/*
 * runtime.c - global daemon state, metrics, and logging
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

struct fdr_runtime fdr;

void
fdr_metrics_init(void)
{
	fdr.metrics = mmap(NULL, sizeof(*fdr.metrics), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (fdr.metrics == MAP_FAILED) {
		perror("mmap metrics");
		exit(FDR_EC_MALLOC);
	}
	memset((void *)fdr.metrics, 0, sizeof(*fdr.metrics));
	fdr.metrics->healthy = 1;
}

void
fdr_metrics_add(volatile uint64_t *counter, uint64_t delta)
{
	*counter += delta;
}

static void
fdr_timestamp(char *buf, size_t buflen)
{
	struct timeval tv;
	struct tm tm;

	gettimeofday(&tv, NULL);
	gmtime_r(&tv.tv_sec, &tm);
	strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

void
fdr_log(const char *level, const char *fmt, ...)
{
	va_list ap;
	char ts[32];
	char msg[1024];

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	fdr_timestamp(ts, sizeof(ts));

	if (fdr.json_log) {
		/* Escape is minimal: drop quotes/newlines in message. */
		char *p;

		for (p = msg; *p; p++) {
			if (*p == '"' || *p == '\n' || *p == '\r')
				*p = ' ';
		}
		fprintf(stderr,
		    "{\"ts\":\"%s\",\"level\":\"%s\",\"msg\":\"%s\",\"pid\":%d}\n",
		    ts, level, msg, (int)getpid());
	} else {
		fprintf(stderr, "%s [%s] %s\n", ts, level, msg);
	}
}

void
fdr_die(int code, const char *fmt, ...)
{
	va_list ap;
	char msg[1024];

	if (fmt != NULL) {
		va_start(ap, fmt);
		vsnprintf(msg, sizeof(msg), fmt, ap);
		va_end(ap);
		fdr_log("error", "%s", msg);
	}
	exit(code);
}

void
fdr_warn(const char *fmt, ...)
{
	va_list ap;
	char msg[1024];

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fdr_log("warn", "%s", msg);
}
