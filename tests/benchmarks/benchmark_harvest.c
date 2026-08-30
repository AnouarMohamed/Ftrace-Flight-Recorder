/*
 * benchmark_harvest.c - Micro-benchmark measuring harvest read/write throughput and CPU efficiency
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Benchmark Architecture & Methodology:
 * 1. Test Workload Generation: Generates a deterministic pseudo-random byte stream
 *    (default: 64 MiB, configurable via `FDR_BENCH_BYTES` environment variable).
 * 2. High-Precision Profiling: Measures both wall-clock time (`CLOCK_MONOTONIC`)
 *    and process CPU execution time (`CLOCK_PROCESS_CPUTIME_ID`) across `fdr_harvest_run()`.
 * 3. End-to-End Integrity Verification: Compares input mock `trace_pipe` against output
 *    `output.log` byte-for-byte in streaming 64 KiB chunks to ensure 100% fidelity.
 * 4. Metric Reporting: Calculates and prints wall-clock nanoseconds, CPU execution nanoseconds,
 *    and effective transfer throughput in MiB/s.
 */

#include "fdr.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/** Chunk size for I/O generation and comparison buffers (64 KiB). */
#define BENCH_CHUNK (64U * 1024U)

/** Default benchmark transfer payload volume (64 MiB). */
#define BENCH_BYTES_DEFAULT (64U * 1024U * 1024U)

/**
 * elapsed_ns - Calculates elapsed nanoseconds between two timespec timestamps.
 *
 * Handles nanosecond underflow normalization when `finish->tv_nsec < start->tv_nsec`.
 *
 * @start: Starting timestamp.
 * @finish: Completion timestamp.
 * Return: Elapsed time in nanoseconds.
 */
static uint64_t
elapsed_ns(const struct timespec *start, const struct timespec *finish)
{
	uint64_t seconds = (uint64_t)(finish->tv_sec - start->tv_sec);
	long nanoseconds = finish->tv_nsec - start->tv_nsec;

	if (nanoseconds < 0) {
		seconds--;
		nanoseconds += 1000000000L;
	}
	return seconds * UINT64_C(1000000000) + (uint64_t)nanoseconds;
}

/**
 * benchmark_bytes - Resolves target benchmark volume from environment or default.
 *
 * Parses `FDR_BENCH_BYTES` if present, otherwise returns `BENCH_BYTES_DEFAULT`.
 *
 * Return: Total byte count to generate and transfer.
 */
static uint64_t
benchmark_bytes(void)
{
	const char *value = getenv("FDR_BENCH_BYTES");
	char *end;
	unsigned long long parsed;

	if (value == NULL || *value == '\0')
		return BENCH_BYTES_DEFAULT;

	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed == 0)
		return 0;

	return (uint64_t)parsed;
}

/**
 * create_input - Generates a synthetic test payload using deterministic arithmetic.
 *
 * @path: Destination file path (mock trace_pipe).
 * @bytes: Total byte volume to write.
 * Return: 0 on success, or -1 on write/close failure.
 */
static int
create_input(const char *path, uint64_t bytes)
{
	unsigned char buffer[BENCH_CHUNK];
	uint64_t offset = 0;
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;

	while (offset < bytes) {
		size_t amount = sizeof(buffer);
		size_t i;

		if (bytes - offset < amount)
			amount = (size_t)(bytes - offset);
		for (i = 0; i < amount; i++)
			buffer[i] = (unsigned char)(((offset + i) * 131U + 17U) & 0xffU);
		if (fdr_write_all(fd, buffer, amount) != 0) {
			(void)close(fd);
			return -1;
		}
		offset += amount;
	}
	return close(fd);
}

/**
 * files_equal - Compares two files byte-by-byte using 64 KiB streaming buffers.
 *
 * @left: Path to first file.
 * @right: Path to second file.
 * Return: 1 if both files exist and are byte-for-byte identical, 0 otherwise.
 */
static int
files_equal(const char *left, const char *right)
{
	unsigned char left_buffer[BENCH_CHUNK];
	unsigned char right_buffer[BENCH_CHUNK];
	int left_fd = -1;
	int right_fd = -1;
	int equal = 0;

	left_fd = open(left, O_RDONLY | O_CLOEXEC);
	right_fd = open(right, O_RDONLY | O_CLOEXEC);
	if (left_fd < 0 || right_fd < 0)
		goto out;

	for (;;) {
		ssize_t left_n = read(left_fd, left_buffer, sizeof(left_buffer));
		ssize_t right_n = read(right_fd, right_buffer, sizeof(right_buffer));

		if (left_n < 0 || right_n < 0)
			goto out;
		if (left_n != right_n)
			goto out;
		if (left_n == 0) {
			equal = 1;
			break;
		}
		if (memcmp(left_buffer, right_buffer, (size_t)left_n) != 0)
			goto out;
	}

out:
	if (left_fd >= 0)
		(void)close(left_fd);
	if (right_fd >= 0)
		(void)close(right_fd);
	return equal;
}

int
main(void)
{
	char tempdir[] = "/tmp/fdr-benchmark-XXXXXX";
	char tracepipe[FDR_PATH_MAX];
	char logfile[FDR_PATH_MAX];
	struct fdr_instance instance;
	struct fdr_item item;
	struct timespec wall_start;
	struct timespec wall_finish;
	struct timespec cpu_start;
	struct timespec cpu_finish;
	uint64_t bytes = benchmark_bytes();
	uint64_t wall_ns;
	uint64_t cpu_ns;
	double throughput;
	int rc;

	if (bytes == 0 || bytes >= UINT64_MAX / 2) {
		fprintf(stderr, "invalid FDR_BENCH_BYTES\n");
		return 2;
	}
	assert(mkdtemp(tempdir) != NULL);
	assert(fdr_join_path(tracepipe, sizeof(tracepipe), tempdir,
	    "trace_pipe") == 0);
	assert(fdr_join_path(logfile, sizeof(logfile), tempdir,
	    "output.log") == 0);
	assert(create_input(tracepipe, bytes) == 0);

	fdr_instance_init(&instance);
	assert(fdr_copy_field(instance.iname, sizeof(instance.iname),
	    "fdr-benchmark") == 0);
	assert(fdr_copy_field(instance.dname, sizeof(instance.dname), tempdir) == 0);
	instance.minfree = 0;
	instance.maxsize = bytes * 2;
	memset(&item, 0, sizeof(item));
	item.type = FDR_ITEM_SAVETO;
	assert(fdr_copy_field(item.target, sizeof(item.target), logfile) == 0);

	fdr_metrics_init();

	/* Capture start timestamps */
	assert(clock_gettime(CLOCK_MONOTONIC, &wall_start) == 0);
	assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_start) == 0);

	/* Execute harvest loop until EOF */
	rc = fdr_harvest_run(&instance, &item);

	/* Capture finish timestamps */
	assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_finish) == 0);
	assert(clock_gettime(CLOCK_MONOTONIC, &wall_finish) == 0);

	/* Integrity and correctness assertions */
	assert(rc == 0);
	assert(fdr_metrics_load_u64(&fdr.metrics->bytes_written) == bytes);
	assert(fdr_metrics_load_u64(&fdr.metrics->bytes_dropped) == 0);
	assert(files_equal(tracepipe, logfile));

	/* Calculate benchmark statistics */
	wall_ns = elapsed_ns(&wall_start, &wall_finish);
	cpu_ns = elapsed_ns(&cpu_start, &cpu_finish);
	throughput = wall_ns == 0 ? 0.0 :
	    ((double)bytes / (1024.0 * 1024.0)) /
	    ((double)wall_ns / 1000000000.0);

	printf("bytes=%" PRIu64 " wall_ns=%" PRIu64 " cpu_ns=%" PRIu64
	    " throughput_mib_s=%.2f\n", bytes, wall_ns, cpu_ns, throughput);

	fdr_metrics_destroy();
	assert(unlink(tracepipe) == 0);
	assert(unlink(logfile) == 0);
	assert(rmdir(tempdir) == 0);
	return 0;
}

