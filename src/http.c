/*
 * http.c - health and Prometheus metrics endpoint
 */

#include "fdr.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int
fdr_http_listen(int port)
{
	int fd, on = 1;
	struct sockaddr_in addr;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((uint16_t)port);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		/* Fall back to all interfaces if loopback bind fails in some ns. */
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			perror("bind");
			close(fd);
			return -1;
		}
	}
	if (listen(fd, 16) < 0) {
		perror("listen");
		close(fd);
		return -1;
	}
	return fd;
}

static void
fdr_http_reply(int cfd, int status, const char *ctype, const char *body)
{
	char hdr[256];
	size_t blen = strlen(body);

	snprintf(hdr, sizeof(hdr),
	    "HTTP/1.1 %d OK\r\n"
	    "Content-Type: %s\r\n"
	    "Content-Length: %zu\r\n"
	    "Connection: close\r\n"
	    "\r\n",
	    status, ctype, blen);
	(void)write(cfd, hdr, strlen(hdr));
	(void)write(cfd, body, blen);
}

static void
fdr_http_handle(int cfd)
{
	char req[1024];
	char body[4096];
	ssize_t n;
	int healthy;
	int workers;

	n = read(cfd, req, sizeof(req) - 1);
	if (n <= 0) {
		close(cfd);
		return;
	}
	req[n] = '\0';

	fdr_process_reap_children();
	workers = fdr.num_children;
	healthy = (fdr.metrics != NULL && fdr.metrics->healthy &&
	    workers >= 0 && !fdr.want_exit);

	if (strncmp(req, "GET /healthz", 12) == 0 ||
	    strncmp(req, "GET /readyz", 11) == 0) {
		if (healthy) {
			fdr_http_reply(cfd, 200, "text/plain", "ok\n");
		} else {
			fdr_http_reply(cfd, 503, "text/plain", "not ready\n");
		}
	} else if (strncmp(req, "GET /metrics", 12) == 0) {
		snprintf(body, sizeof(body),
		    "# HELP fdr_bytes_written_total Bytes written to save files\n"
		    "# TYPE fdr_bytes_written_total counter\n"
		    "fdr_bytes_written_total %llu\n"
		    "# HELP fdr_bytes_dropped_total Bytes dropped (disk/rate limit)\n"
		    "# TYPE fdr_bytes_dropped_total counter\n"
		    "fdr_bytes_dropped_total %llu\n"
		    "# HELP fdr_rotations_total Log rotations performed\n"
		    "# TYPE fdr_rotations_total counter\n"
		    "fdr_rotations_total %llu\n"
		    "# HELP fdr_probe_failures_total Missing or failed probes\n"
		    "# TYPE fdr_probe_failures_total counter\n"
		    "fdr_probe_failures_total %llu\n"
		    "# HELP fdr_rate_limit_drops_total Bytes dropped by ratelimit\n"
		    "# TYPE fdr_rate_limit_drops_total counter\n"
		    "fdr_rate_limit_drops_total %llu\n"
		    "# HELP fdr_write_errors_total Write errors\n"
		    "# TYPE fdr_write_errors_total counter\n"
		    "fdr_write_errors_total %llu\n"
		    "# HELP fdr_reloads_total Config reloads\n"
		    "# TYPE fdr_reloads_total counter\n"
		    "fdr_reloads_total %llu\n"
		    "# HELP fdr_instances Configured instances\n"
		    "# TYPE fdr_instances gauge\n"
		    "fdr_instances %d\n"
		    "# HELP fdr_workers_alive Running worker processes\n"
		    "# TYPE fdr_workers_alive gauge\n"
		    "fdr_workers_alive %d\n"
		    "# HELP fdr_healthy 1 if healthy\n"
		    "# TYPE fdr_healthy gauge\n"
		    "fdr_healthy %d\n",
		    (unsigned long long)(fdr.metrics ? fdr.metrics->bytes_written : 0),
		    (unsigned long long)(fdr.metrics ? fdr.metrics->bytes_dropped : 0),
		    (unsigned long long)(fdr.metrics ? fdr.metrics->rotations : 0),
		    (unsigned long long)(fdr.metrics ? fdr.metrics->probe_failures : 0),
		    (unsigned long long)(fdr.metrics ? fdr.metrics->rate_limit_drops : 0),
		    (unsigned long long)(fdr.metrics ? fdr.metrics->write_errors : 0),
		    (unsigned long long)(fdr.metrics ? fdr.metrics->reloads : 0),
		    fdr.instance_count,
		    workers,
		    healthy ? 1 : 0);
		fdr_http_reply(cfd, 200, "text/plain; version=0.0.4", body);
	} else {
		fdr_http_reply(cfd, 404, "text/plain", "not found\n");
	}
	close(cfd);
}

int
fdr_http_serve(int port)
{
	int lfd;
	struct pollfd pfd;

	lfd = fdr_http_listen(port);
	if (lfd < 0)
		return -1;

	fdr_log("info", "http listening on port %d (/healthz /readyz /metrics)",
	    port);

	pfd.fd = lfd;
	pfd.events = POLLIN;

	while (!fdr.want_exit) {
		if (fdr.want_reload) {
			fdr.want_reload = 0;
			if (fdr_process_reload() != 0)
				fdr_warn("config reload failed; keeping previous workers if any");
		}

		pfd.revents = 0;
		if (poll(&pfd, 1, 500) < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		fdr_process_reap_children();

		if (pfd.revents & POLLIN) {
			int cfd = accept(lfd, NULL, NULL);

			if (cfd >= 0)
				fdr_http_handle(cfd);
		}
	}

	close(lfd);
	return 0;
}
