/*
 * runtime.c - Global daemon runtime state, shared metrics, and structured logging
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Overview:
 * This module manages the lifecycle of shared daemon state and diagnostics:
 * 1. Global Daemon Runtime: Instantiates `struct fdr_runtime fdr` holding
 *    process state, signal flags, and the supervisor child process table.
 * 2. Inter-Process Shared Metrics: Allocates and tears down an anonymous
 *    shared memory page (mmap MAP_SHARED | MAP_ANONYMOUS). Both the parent
 *    supervisor and worker children write/read telemetry counters using
 *    relaxed memory order atomics (__atomic_*).
 * 3. Structured Logging Pipeline: Implements formatted output (fdr_log,
 *    fdr_warn, fdr_die) supporting both human-readable plain text and
 *    machine-readable JSON (with full string escaping) with RFC 3339 UTC timestamps.
 */

#include "fdr.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/** Global daemon singleton holding runtime state, flags, and child PID table. */
struct fdr_runtime fdr;

/**
 * fdr_metrics_init - Allocates the shared memory segment for Prometheus metrics.
 *
 * Uses mmap with MAP_SHARED | MAP_ANONYMOUS so that both the parent supervisor
 * and worker children forked afterward share the exact same physical memory page.
 * Initializes all metric counters to zero and sets initial readiness to healthy (1).
 *
 * Aborts the process with FDR_EC_MALLOC if allocation fails.
 */
void
fdr_metrics_init(void)
{
	fdr.metrics = mmap(NULL, sizeof(*fdr.metrics), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (fdr.metrics == MAP_FAILED) {
		fdr.metrics = NULL;
		fdr_die(FDR_EC_MALLOC, "cannot allocate shared metrics");
	}
	memset(fdr.metrics, 0, sizeof(*fdr.metrics));
	/* Default to healthy readiness state upon clean startup */
	fdr_metrics_store_int(&fdr.metrics->healthy, 1);
}

/**
 * fdr_metrics_destroy - Unmaps the shared metrics segment upon daemon termination.
 *
 * Releases the shared memory mapping via munmap(2) and clears the pointer.
 */
void
fdr_metrics_destroy(void)
{
	if (fdr.metrics != NULL) {
		(void)munmap(fdr.metrics, sizeof(*fdr.metrics));
		fdr.metrics = NULL;
	}
}

/**
 * fdr_metrics_add - Atomically adds a delta to a 64-bit shared metric counter.
 *
 * Uses __ATOMIC_RELAXED ordering because these cumulative telemetry counters
 * do not enforce memory synchronization barriers for other variables.
 *
 * @counter: Pointer to uint64_t counter in shared memory.
 * @delta: Value to add to the counter.
 */
void
fdr_metrics_add(uint64_t *counter, uint64_t delta)
{
	(void)__atomic_fetch_add(counter, delta, __ATOMIC_RELAXED);
}

/**
 * fdr_metrics_load_u64 - Atomically loads a 64-bit shared metric counter.
 *
 * @counter: Pointer to uint64_t counter in shared memory.
 * Return: Current 64-bit counter value.
 */
uint64_t
fdr_metrics_load_u64(const uint64_t *counter)
{
	return __atomic_load_n(counter, __ATOMIC_RELAXED);
}

/**
 * fdr_metrics_load_int - Atomically loads a 32-bit integer metric gauge.
 *
 * @value: Pointer to int gauge in shared memory.
 * Return: Current integer gauge value.
 */
int
fdr_metrics_load_int(const int *value)
{
	return __atomic_load_n(value, __ATOMIC_RELAXED);
}

/**
 * fdr_metrics_store_int - Atomically sets a 32-bit integer metric gauge.
 *
 * @value: Pointer to int gauge in shared memory.
 * @new_value: Value to store.
 */
void
fdr_metrics_store_int(int *value, int new_value)
{
	__atomic_store_n(value, new_value, __ATOMIC_RELAXED);
}

/**
 * fdr_timestamp - Generates an RFC 3339 UTC timestamp string (e.g. "2026-08-30T01:00:00Z").
 *
 * @buf: Destination character buffer.
 * @buflen: Size of destination buffer (should be at least 32 bytes).
 */
static void
fdr_timestamp(char *buf, size_t buflen)
{
	struct timeval tv;
	struct tm tm;

	(void)gettimeofday(&tv, NULL);
	(void)gmtime_r(&tv.tv_sec, &tm);
	(void)strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/**
 * fdr_json_escape - Escapes special characters for valid JSON string encoding.
 *
 * Converts quotation marks ("), backslashes (\), and control characters (\n, \r, \t)
 * into their respective JSON escape sequences. Filters out non-printable ASCII (< 0x20).
 *
 * @dst: Destination buffer for escaped JSON string.
 * @dstsz: Total capacity of destination buffer.
 * @src: Raw null-terminated input string.
 */
static void
fdr_json_escape(char *dst, size_t dstsz, const char *src)
{
	size_t used = 0;
	unsigned char ch;

	if (dstsz == 0)
		return;

	while ((ch = (unsigned char)*src++) != '\0' && used + 1 < dstsz) {
		const char *escaped = NULL;

		switch (ch) {
		case '"': escaped = "\\\""; break;
		case '\\': escaped = "\\\\"; break;
		case '\n': escaped = "\\n"; break;
		case '\r': escaped = "\\r"; break;
		case '\t': escaped = "\\t"; break;
		default: break;
		}

		if (escaped != NULL) {
			size_t n = strlen(escaped);

			if (used + n >= dstsz)
				break;
			memcpy(dst + used, escaped, n);
			used += n;
		} else if (ch >= 0x20) {
			dst[used++] = (char)ch;
		}
	}
	dst[used] = '\0';
}

/**
 * fdr_log - Emits a log message with level and timestamp to stderr.
 *
 * Respects `fdr.json_log`:
 * - JSON mode (-j): Formats as {"ts":"...","level":"...","msg":"...","pid":...}
 * - Plain text mode: Formats as "YYYY-MM-DDTHH:MM:SSZ [level] message"
 *
 * @level: Severity level ("info", "warn", "error").
 * @fmt: Printf format string.
 */
void
fdr_log(const char *level, const char *fmt, ...)
{
	va_list ap;
	char ts[32];
	char msg[1024];

	va_start(ap, fmt);
	(void)vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fdr_timestamp(ts, sizeof(ts));

	if (fdr.json_log) {
		char escaped[2048];

		fdr_json_escape(escaped, sizeof(escaped), msg);
		fprintf(stderr,
		    "{\"ts\":\"%s\",\"level\":\"%s\",\"msg\":\"%s\",\"pid\":%d}\n",
		    ts, level, escaped, (int)getpid());
	} else {
		fprintf(stderr, "%s [%s] %s\n", ts, level, msg);
	}
}

/**
 * fdr_die - Logs a fatal error message and terminates the process with an exit code.
 *
 * @code: Exit status code (typically one of FDR_EC_*).
 * @fmt: Optional printf format string (if NULL, exits without logging).
 */
void
fdr_die(int code, const char *fmt, ...)
{
	va_list ap;
	char msg[1024];

	if (fmt != NULL) {
		va_start(ap, fmt);
		(void)vsnprintf(msg, sizeof(msg), fmt, ap);
		va_end(ap);
		fdr_log("error", "%s", msg);
	}
	exit(code);
}

/**
 * fdr_warn - Logs a non-fatal warning message to stderr.
 *
 * @fmt: Printf format string.
 */
void
fdr_warn(const char *fmt, ...)
{
	va_list ap;
	char msg[1024];

	va_start(ap, fmt);
	(void)vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fdr_log("warn", "%s", msg);
}

