/*
 * http.c - HTTP health, forensic readiness, Prometheus telemetry, and parent event loop
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Overview:
 * This module implements the parent supervisor's primary multiplexer event loop:
 * 1. Non-Blocking HTTP Server: Binds to an IPv4 socket (`127.0.0.1:9119` or pod IP)
 *    and handles short-lived HTTP GET requests via non-blocking `accept4(SOCK_CLOEXEC)`.
 * 2. Operational Endpoints:
 *    - `/healthz`: Liveness probe (returns 200 "ok" while the event loop is active).
 *    - `/readyz`: Forensic readiness probe (returns 200 "ready" when evidence integrity
 *      is intact, or 503 "not ready" if probe failures, write errors, or kernel
 *      trace losses were detected).
 *    - `/metrics`: Cumulative Prometheus metrics exposition (v0.0.4 format).
 * 3. Supervisor Multiplexer: Uses poll(2) with a 500ms timeout to coordinate:
 *    - Child process reaping upon receiving `SIGCHLD`.
 *    - Transactional configuration reload upon receiving `SIGHUP`.
 *    - Periodic 5-second sampling of kernel trace-loss statistics across all CPUs.
 *    - Clean daemon shutdown upon receiving `SIGTERM` or `SIGINT`.
 */

#include "fdr.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/** Sampling interval in seconds for querying kernel per-CPU trace loss counters. */
#define FDR_TRACE_STATS_INTERVAL 5

/**
 * fdr_trace_stats_due - Checks if the 5-second kernel loss sampling interval has expired.
 *
 * Compares current monotonic time against `next`. If due, advances `next` by
 * `FDR_TRACE_STATS_INTERVAL` seconds.
 *
 * @next: In/out pointer to monotonic timespec tracking next due timestamp.
 * Return: 1 if sampling is due, 0 otherwise.
 */
static int
fdr_trace_stats_due(struct timespec *next)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	if (now.tv_sec < next->tv_sec ||
	    (now.tv_sec == next->tv_sec && now.tv_nsec < next->tv_nsec))
		return 0;

	*next = now;
	next->tv_sec += FDR_TRACE_STATS_INTERVAL;
	return 1;
}

/**
 * fdr_http_listen - Creates, binds, and configures a non-blocking TCP IPv4 listening socket.
 *
 * Socket configuration:
 * - AF_INET, SOCK_STREAM | SOCK_CLOEXEC.
 * - SO_REUSEADDR enabled to allow immediate rebinding after restart.
 * - O_NONBLOCK set via fcntl so accept4 loops do not block the event multiplexer.
 *
 * @address: IPv4 bind address string (e.g. "127.0.0.1" or "0.0.0.0").
 * @port: TCP port number (e.g. 9119).
 * Return: Open listening socket file descriptor >= 0, or -1 on network error.
 */
static int
fdr_http_listen(const char *address, int port)
{
	int fd;
	int on = 1;
	int flags;
	struct sockaddr_in addr;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);

	if (inet_pton(AF_INET, address, &addr.sin_addr) != 1) {
		errno = EINVAL;
		close(fd);
		return -1;
	}

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(fd, 32) != 0) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}

	return fd;
}

/**
 * fdr_http_reply - Formats and sends an HTTP/1.1 response with security headers.
 *
 * Sends `HTTP/1.1 <status> <reason>`, `Content-Type: <ctype>`, `Content-Length`,
 * `Connection: close`, and `X-Content-Type-Options: nosniff`, followed by `body`.
 *
 * @cfd: Connected client socket descriptor.
 * @status: HTTP numeric status code (200, 400, 404, 405, 503).
 * @reason: HTTP status text ("OK", "Bad Request", "Service Unavailable").
 * @ctype: Content-Type header string (e.g. "text/plain").
 * @body: Null-terminated response payload string.
 * Return: 0 on success, -1 on write failure.
 */
static int
fdr_http_reply(int cfd, int status, const char *reason, const char *ctype,
    const char *body)
{
	char header[512];
	int length;

	length = snprintf(header, sizeof(header),
	    "HTTP/1.1 %d %s\r\n"
	    "Content-Type: %s\r\n"
	    "Content-Length: %zu\r\n"
	    "Connection: close\r\n"
	    "X-Content-Type-Options: nosniff\r\n"
	    "\r\n",
	    status, reason, ctype, strlen(body));

	if (length < 0 || (size_t)length >= sizeof(header))
		return -1;

	if (fdr_write_all(cfd, header, (size_t)length) != 0 ||
	    fdr_write_all(cfd, body, strlen(body)) != 0)
		return -1;

	return 0;
}

/**
 * fdr_http_handle - Parses incoming HTTP client request and dispatches response.
 *
 * Enforces a 2-second send/receive socket timeout, parses method and URL path,
 * validates GET method, and serves:
 * - `/healthz`: 200 "ok" if alive, 503 "stopping" if shutdown requested.
 * - `/readyz`: 200 "ready" if `fdr.metrics->healthy == 1`, 503 "not ready" if degraded.
 * - `/metrics`: Formats all shared counters and gauges into Prometheus text exposition.
 * - Unknown paths: 404 "not found".
 *
 * @cfd: Connected client socket descriptor.
 */
static void
fdr_http_handle(int cfd)
{
	char request[1024];
	char method[8];
	char path[256];
	char version[16];
	char body[4096];
	struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
	ssize_t n;
	int alive;
	int ready;

	/* Set socket timeouts to protect against stalled HTTP clients */
	(void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	(void)setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	n = read(cfd, request, sizeof(request) - 1);
	if (n <= 0)
		return;

	request[n] = '\0';
	if (sscanf(request, "%7s %255s %15s", method, path, version) != 3) {
		(void)fdr_http_reply(cfd, 400, "Bad Request", "text/plain",
		    "bad request\n");
		return;
	}

	if (strcmp(method, "GET") != 0) {
		(void)fdr_http_reply(cfd, 405, "Method Not Allowed", "text/plain",
		    "method not allowed\n");
		return;
	}

	alive = !fdr.want_exit;
	ready = alive && fdr.metrics != NULL &&
	    fdr_metrics_load_int(&fdr.metrics->healthy);

	if (strcmp(path, "/healthz") == 0) {
		(void)fdr_http_reply(cfd, alive ? 200 : 503,
		    alive ? "OK" : "Service Unavailable", "text/plain",
		    alive ? "ok\n" : "stopping\n");
	} else if (strcmp(path, "/readyz") == 0) {
		(void)fdr_http_reply(cfd, ready ? 200 : 503,
		    ready ? "OK" : "Service Unavailable", "text/plain",
		    ready ? "ready\n" : "not ready\n");
	} else if (strcmp(path, "/metrics") == 0) {
		const struct fdr_metrics *m = fdr.metrics;

		(void)snprintf(body, sizeof(body),
		    "# HELP fdr_bytes_written_total Bytes written to save files\n"
		    "# TYPE fdr_bytes_written_total counter\n"
		    "fdr_bytes_written_total %" PRIu64 "\n"
		    "# HELP fdr_bytes_dropped_total Bytes dropped to protect storage\n"
		    "# TYPE fdr_bytes_dropped_total counter\n"
		    "fdr_bytes_dropped_total %" PRIu64 "\n"
		    "# HELP fdr_rotations_total Log rotations performed\n"
		    "# TYPE fdr_rotations_total counter\n"
		    "fdr_rotations_total %" PRIu64 "\n"
		    "# HELP fdr_rotation_failures_total Failed log rotation attempts\n"
		    "# TYPE fdr_rotation_failures_total counter\n"
		    "fdr_rotation_failures_total %" PRIu64 "\n"
		    "# HELP fdr_probe_failures_total Probe configuration failures\n"
		    "# TYPE fdr_probe_failures_total counter\n"
		    "fdr_probe_failures_total %" PRIu64 "\n"
		    "# HELP fdr_write_errors_total Log write failures\n"
		    "# TYPE fdr_write_errors_total counter\n"
		    "fdr_write_errors_total %" PRIu64 "\n"
		    "# HELP fdr_reloads_total Successful configuration reloads\n"
		    "# TYPE fdr_reloads_total counter\n"
		    "fdr_reloads_total %" PRIu64 "\n"
		    "# HELP fdr_trace_overruns_total Events lost by ring-buffer overwrite\n"
		    "# TYPE fdr_trace_overruns_total counter\n"
		    "fdr_trace_overruns_total %" PRIu64 "\n"
		    "# HELP fdr_trace_dropped_events_total New events dropped by tracefs\n"
		    "# TYPE fdr_trace_dropped_events_total counter\n"
		    "fdr_trace_dropped_events_total %" PRIu64 "\n"
		    "# HELP fdr_trace_commit_overruns_total Nested writes lost by tracefs\n"
		    "# TYPE fdr_trace_commit_overruns_total counter\n"
		    "fdr_trace_commit_overruns_total %" PRIu64 "\n"
		    "# HELP fdr_instances Configured instances\n"
		    "# TYPE fdr_instances gauge\n"
		    "fdr_instances %d\n"
		    "# HELP fdr_workers_alive Running worker processes\n"
		    "# TYPE fdr_workers_alive gauge\n"
		    "fdr_workers_alive %d\n"
		    "# HELP fdr_ready 1 when known evidence integrity is intact\n"
		    "# TYPE fdr_ready gauge\n"
		    "fdr_ready %d\n",
		    m ? fdr_metrics_load_u64(&m->bytes_written) : 0,
		    m ? fdr_metrics_load_u64(&m->bytes_dropped) : 0,
		    m ? fdr_metrics_load_u64(&m->rotations) : 0,
		    m ? fdr_metrics_load_u64(&m->rotation_failures) : 0,
		    m ? fdr_metrics_load_u64(&m->probe_failures) : 0,
		    m ? fdr_metrics_load_u64(&m->write_errors) : 0,
		    m ? fdr_metrics_load_u64(&m->reloads) : 0,
		    m ? fdr_metrics_load_u64(&m->trace_overruns) : 0,
		    m ? fdr_metrics_load_u64(&m->trace_dropped_events) : 0,
		    m ? fdr_metrics_load_u64(&m->trace_commit_overruns) : 0,
		    m ? fdr_metrics_load_int(&m->instances) : 0,
		    m ? fdr_metrics_load_int(&m->workers_alive) : 0,
		    ready ? 1 : 0);
		(void)fdr_http_reply(cfd, 200, "OK",
		    "text/plain; version=0.0.4; charset=utf-8", body);
	} else {
		(void)fdr_http_reply(cfd, 404, "Not Found", "text/plain",
		    "not found\n");
	}
}

/**
 * fdr_http_serve - Main supervisor event loop: multiplexes signals, loss stats, and HTTP.
 *
 * Lifecycle:
 * 1. Opens HTTP listener if port != 0.
 * 2. Loops until `fdr.want_exit` is set:
 *    - Reaps finished children if `fdr.got_sigchld` is set.
 *    - Reloads configuration if `fdr.want_reload` is set.
 *    - Samples kernel trace loss across instances every 5 seconds.
 *    - Polls HTTP listener socket with a 500ms timeout.
 *    - Non-blocking accepts all pending HTTP connections using `accept4(SOCK_CLOEXEC)`.
 * 3. Closes listening socket on shutdown.
 *
 * @address: Bind IP address string.
 * @port: Port number (0 disables HTTP server).
 * Return: Exit status code (0 for normal exit, FDR_EC_HTTP on network error).
 */
int
fdr_http_serve(const char *address, int port)
{
	int listener = -1;
	struct pollfd pfd;
	struct timespec next_trace_stats = { 0, 0 };

	if (port != 0) {
		listener = fdr_http_listen(address, port);
		if (listener < 0) {
			fdr_warn("cannot listen on %s:%d: %s", address, port,
			    strerror(errno));
			return FDR_EC_HTTP;
		}
		fdr_log("info",
		    "HTTP endpoints listening on %s:%d (/healthz /readyz /metrics)",
		    address, port);
	} else {
		fdr_log("info", "HTTP endpoints disabled");
	}

	pfd.fd = listener;
	pfd.events = POLLIN;

	while (!fdr.want_exit) {
		int poll_rc;

		/* Check and reap child worker processes */
		if (fdr.got_sigchld)
			fdr_process_reap_children();

		/* Check and execute pending configuration reload */
		if (fdr.want_reload && !fdr.want_exit) {
			fdr.want_reload = 0;
			if (fdr_process_reload() != 0)
				fdr_warn("configuration reload rejected; current configuration remains active");
		}

		if (fdr.want_exit)
			break;

		/* Check periodic kernel loss sampling schedule */
		if (fdr_trace_stats_due(&next_trace_stats))
			fdr_trace_sample_all_loss();

		pfd.revents = 0;
		poll_rc = poll(listener >= 0 ? &pfd : NULL,
		    listener >= 0 ? 1U : 0U, 500);

		if (poll_rc < 0) {
			if (errno == EINTR)
				continue;
			fdr_warn("HTTP event loop failed: %s", strerror(errno));
			fdr.exit_status = FDR_EC_HTTP;
			break;
		}

		/* Accept incoming HTTP client connections */
		if (listener >= 0 && (pfd.revents & POLLIN)) {
			for (;;) {
				int cfd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

				if (cfd < 0) {
					if (errno != EAGAIN && errno != EWOULDBLOCK &&
					    errno != EINTR)
						fdr_warn("HTTP accept failed: %s",
						    strerror(errno));
					break;
				}
				fdr_http_handle(cfd);
				close(cfd);
			}
		}
	}

	if (listener >= 0)
		close(listener);
	return fdr.exit_status;
}

