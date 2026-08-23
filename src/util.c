/*
 * util.c - string, path, numeric, and I/O helpers
 */

#include "fdr.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
fdr_copy_field(char *dst, size_t dstsz, const char *src)
{
	int n;

	if (dstsz == 0)
		return -1;
	n = snprintf(dst, dstsz, "%s", src);
	return n < 0 || (size_t)n >= dstsz ? -1 : 0;
}

int
fdr_join_path(char *dst, size_t dstsz, const char *a, const char *b)
{
	int n = snprintf(dst, dstsz, "%s/%s", a, b);

	return n < 0 || (size_t)n >= dstsz ? -1 : 0;
}

void
fdr_chomp_line(char *buf)
{
	size_t len = strlen(buf);

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';
}

int
fdr_parse_size(const char *arg, uint64_t *value)
{
	unsigned long long parsed;
	uint64_t multiplier = 1;
	char *end;

	if (arg == NULL || *arg == '\0' || *arg == '-' || value == NULL)
		return -1;
	errno = 0;
	parsed = strtoull(arg, &end, 10);
	if (errno != 0 || end == arg)
		return -1;

	if (*end != '\0') {
		switch (tolower((unsigned char)*end++)) {
		case 'k': multiplier = UINT64_C(1024); break;
		case 'm': multiplier = UINT64_C(1024) * 1024; break;
		case 'g': multiplier = UINT64_C(1024) * 1024 * 1024; break;
		default: return -1;
		}
		if (*end == 'i' || *end == 'I')
			end++;
		if (*end == 'b' || *end == 'B')
			end++;
		if (*end != '\0')
			return -1;
	}

	if ((uint64_t)parsed > UINT64_MAX / multiplier)
		return -1;
	*value = (uint64_t)parsed * multiplier;
	return 0;
}

const char *
fdr_default_inst_dir(void)
{
	if (access(FDR_TRACE_INST_DIR, F_OK) == 0)
		return FDR_TRACE_INST_DIR;
	if (access(FDR_DEBUG_INST_DIR, F_OK) == 0)
		return FDR_DEBUG_INST_DIR;
	return FDR_TRACE_INST_DIR;
}

int
fdr_write_all(int fd, const void *buf, size_t len)
{
	const char *cursor = buf;

	while (len > 0) {
		ssize_t written = write(fd, cursor, len);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (written == 0) {
			errno = EIO;
			return -1;
		}
		cursor += written;
		len -= (size_t)written;
	}
	return 0;
}

void
fdr_instance_init(struct fdr_instance *insp)
{
	memset(insp, 0, sizeof(*insp));
	insp->minfree = FDR_MINFREE_DEFAULT;
	insp->maxsize = UINT64_MAX;
}

void
fdr_instance_append(struct fdr_instance *insp)
{
	struct fdr_instance **tail = &fdr.instances;

	while (*tail != NULL)
		tail = &(*tail)->next;
	*tail = insp;
	fdr.instance_count++;
}
