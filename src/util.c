/*
 * util.c - String, path, numeric parsing, and low-level I/O helpers
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Overview:
 * This module provides foundational, defensive system utilities used across
 * both the supervisor parent process and worker children in FDR:
 * - Safe, bounded string formatting and path concatenation to prevent buffer overflows.
 * - String sanitization (stripping trailing whitespace/newlines).
 * - Human-readable size parser supporting standard binary units (KiB, MiB, GiB)
 *   with strict integer overflow detection.
 * - Dynamic tracefs instance directory auto-discovery.
 * - Robust write-all I/O loop resilient against partial writes and EINTR signals.
 * - Instance state initialization and linked-list manipulation.
 */

#include "fdr.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * fdr_copy_field - Safely copies a source string into a bounded destination buffer.
 *
 * Uses snprintf to guarantee null-termination and verifies that the entire source
 * string fits within the destination buffer without truncation.
 *
 * @dst: Destination character buffer.
 * @dstsz: Total capacity of destination buffer in bytes.
 * @src: Null-terminated source string to copy.
 *
 * Return: 0 on success, or -1 if dstsz is zero or if truncation occurred.
 */
int
fdr_copy_field(char *dst, size_t dstsz, const char *src)
{
	int n;

	if (dstsz == 0)
		return -1;

	n = snprintf(dst, dstsz, "%s", src);
	/* snprintf returns total bytes needed; if n >= dstsz, string was truncated */
	return n < 0 || (size_t)n >= dstsz ? -1 : 0;
}

/**
 * fdr_join_path - Safely concatenates two path components with a separating '/'.
 *
 * Constructs "<a>/<b>" within the specified buffer capacity. Guarantees null-termination
 * and checks for string truncation.
 *
 * @dst: Destination buffer to receive joined path.
 * @dstsz: Total capacity of destination buffer in bytes.
 * @a: First path component (directory).
 * @b: Second path component (filename or subdirectory).
 *
 * Return: 0 on success, or -1 if dstsz is zero or if truncation occurred.
 */
int
fdr_join_path(char *dst, size_t dstsz, const char *a, const char *b)
{
	int n = snprintf(dst, dstsz, "%s/%s", a, b);

	return n < 0 || (size_t)n >= dstsz ? -1 : 0;
}

/**
 * fdr_chomp_line - Trims trailing newline ('\n') and carriage return ('\r') characters.
 *
 * Modifies the provided null-terminated string in-place by overwriting trailing
 * line break characters with null terminators.
 *
 * @buf: Null-terminated string buffer to modify.
 */
void
fdr_chomp_line(char *buf)
{
	size_t len = strlen(buf);

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';
}

/**
 * fdr_parse_size - Parses human-readable memory/disk size strings into byte values.
 *
 * Parses numeric strings with optional binary unit suffixes:
 * - 'k', 'K', 'KB', 'KiB' -> Multiplied by 1024 (2^10)
 * - 'm', 'M', 'MB', 'MiB' -> Multiplied by 1048576 (2^20)
 * - 'g', 'G', 'GB', 'GiB' -> Multiplied by 1073741824 (2^30)
 * - Raw integers -> Interpreted as byte counts.
 *
 * Validates against negative numbers, invalid trailing characters, and 64-bit
 * unsigned integer multiplication overflow.
 *
 * @arg: Null-terminated size string (e.g., "16m", "64k", "1GiB", "4096").
 * @value: Pointer to uint64_t to store the parsed byte count.
 *
 * Return: 0 on successful parse, or -1 on syntax error or integer overflow.
 */
int
fdr_parse_size(const char *arg, uint64_t *value)
{
	unsigned long long parsed;
	uint64_t multiplier = 1;
	char *end;

	/* Reject NULL, empty string, negative signs, or NULL target pointer */
	if (arg == NULL || *arg == '\0' || *arg == '-' || value == NULL)
		return -1;

	errno = 0;
	parsed = strtoull(arg, &end, 10);
	if (errno != 0 || end == arg)
		return -1;

	/* Parse optional unit suffix (case-insensitive) */
	if (*end != '\0') {
		switch (tolower((unsigned char)*end++)) {
		case 'k': multiplier = UINT64_C(1024); break;
		case 'm': multiplier = UINT64_C(1024) * 1024; break;
		case 'g': multiplier = UINT64_C(1024) * 1024 * 1024; break;
		default: return -1;
		}

		/* Optional 'i' / 'I' suffix (e.g., KiB, MiB, GiB) */
		if (*end == 'i' || *end == 'I')
			end++;

		/* Optional 'b' / 'B' suffix (e.g., KB, MB, GB, KiB, MiB, GiB) */
		if (*end == 'b' || *end == 'B')
			end++;

		/* If any trailing unparsed characters remain, reject */
		if (*end != '\0')
			return -1;
	}

	/* Guard against integer overflow during multiplication */
	if ((uint64_t)parsed > UINT64_MAX / multiplier)
		return -1;

	*value = (uint64_t)parsed * multiplier;
	return 0;
}

/**
 * fdr_default_inst_dir - Resolves the host kernel's tracefs instance mount point.
 *
 * Probes the filesystem to locate active ftrace instances:
 * 1. Standard modern tracefs mount: /sys/kernel/tracing/instances
 * 2. Legacy debugfs mount: /sys/kernel/debug/tracing/instances
 *
 * Return: Constant string path to the resolved tracefs instances directory.
 */
const char *
fdr_default_inst_dir(void)
{
	if (access(FDR_TRACE_INST_DIR, F_OK) == 0)
		return FDR_TRACE_INST_DIR;
	if (access(FDR_DEBUG_INST_DIR, F_OK) == 0)
		return FDR_DEBUG_INST_DIR;
	return FDR_TRACE_INST_DIR;
}

/**
 * fdr_write_all - Writes an exact byte buffer to a file descriptor, looping if partial.
 *
 * Repeatedly invokes write(2) until all requested bytes are written. Correctly
 * handles EINTR interrupts caused by incoming signals without dropping data.
 *
 * @fd: Target open file descriptor.
 * @buf: Pointer to byte buffer to write.
 * @len: Total number of bytes to write.
 *
 * Return: 0 on successful complete write, or -1 on unrecoverable write error.
 */
int
fdr_write_all(int fd, const void *buf, size_t len)
{
	const char *cursor = buf;

	while (len > 0) {
		ssize_t written = write(fd, cursor, len);

		if (written < 0) {
			/* Interrupted by signal; retry write */
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (written == 0) {
			/* Zero bytes written indicates unexpected end of stream or full buffer */
			errno = EIO;
			return -1;
		}
		cursor += written;
		len -= (size_t)written;
	}
	return 0;
}

/**
 * fdr_trace_reset_loss_cache - Releases cached per-CPU stats paths.
 *
 * @insp: Target tracefs instance.
 */
void
fdr_trace_reset_loss_cache(struct fdr_instance *insp)
{
	size_t index;

	for (index = 0; index < insp->trace_stats_path_count; index++)
		free(insp->trace_stats_paths[index]);
	free(insp->trace_stats_paths);
	insp->trace_stats_paths = NULL;
	insp->trace_stats_path_count = 0;
	insp->trace_stats_samples = 0;
	insp->trace_stats_online_cpus = 0;
}

/**
 * fdr_instance_init - Sets default values on a new tracefs instance struct.
 *
 * Initializes memory and sets default minfree threshold (5%) and unlimited
 * maximum file size (UINT64_MAX).
 *
 * @insp: Pointer to the instance struct to initialize.
 */
void
fdr_instance_init(struct fdr_instance *insp)
{
	memset(insp, 0, sizeof(*insp));
	insp->minfree = FDR_MINFREE_DEFAULT;
	insp->maxsize = UINT64_MAX;
}

/**
 * fdr_instance_append - Appends an instance to the global daemon linked list.
 *
 * Traverses the global `fdr.instances` linked list, appends `insp` to the tail,
 * and increments `fdr.instance_count`.
 *
 * @insp: Pointer to the initialized and populated instance structure.
 */
void
fdr_instance_append(struct fdr_instance *insp)
{
	struct fdr_instance **tail = &fdr.instances;

	while (*tail != NULL)
		tail = &(*tail)->next;
	*tail = insp;
	fdr.instance_count++;
}
