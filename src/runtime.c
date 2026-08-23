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
		fdr.metrics = NULL;
		fdr_die(FDR_EC_MALLOC, "cannot allocate shared metrics");
	}
	memset(fdr.metrics, 0, sizeof(*fdr.metrics));
	fdr_metrics_store_int(&fdr.metrics->healthy, 1);
}

void
fdr_metrics_destroy(void)
{
	if (fdr.metrics != NULL) {
		(void)munmap(fdr.metrics, sizeof(*fdr.metrics));
		fdr.metrics = NULL;
	}
}

void
fdr_metrics_add(uint64_t *counter, uint64_t delta)
{
	(void)__atomic_fetch_add(counter, delta, __ATOMIC_RELAXED);
}

uint64_t
fdr_metrics_load_u64(const uint64_t *counter)
{
	return __atomic_load_n(counter, __ATOMIC_RELAXED);
}

int
fdr_metrics_load_int(const int *value)
{
	return __atomic_load_n(value, __ATOMIC_RELAXED);
}

void
fdr_metrics_store_int(int *value, int new_value)
{
	__atomic_store_n(value, new_value, __ATOMIC_RELAXED);
}

static void
fdr_timestamp(char *buf, size_t buflen)
{
	struct timeval tv;
	struct tm tm;

	(void)gettimeofday(&tv, NULL);
	(void)gmtime_r(&tv.tv_sec, &tm);
	(void)strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

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
