/*
 * test_harvest.c - Comprehensive unit tests for data-plane harvesting, signal reopen, and rotation
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Test Scope:
 * 1. Disk Space Protection (minfree): Verifies data drops when free space threshold is triggered.
 * 2. Normal Trace Harvesting: Verifies byte-accurate persistence from FIFO/trace_pipe to disk.
 * 3. Log Rotation: Tests bounded size rotation and `.1` backup creation.
 * 4. External Signal Reopen: Verifies SIGHUP/SIGUSR1 handler reopens file after out-of-band rename.
 * 5. Failure Recovery: Simulates rotation failure (read-only filesystem permissions), tests retry
 *    rate-limiting and automatic recovery once permissions are restored.
 */

#include "fdr.h"

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/**
 * write_trace - Writes a test payload to a mock trace_pipe file.
 *
 * @path: Filesystem path to mock trace_pipe.
 * @payload: Data buffer to write.
 */
static void
write_trace(const char *path, const char *payload)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	assert(fd >= 0);
	assert(fdr_write_all(fd, payload, strlen(payload)) == 0);
	assert(close(fd) == 0);
}

/**
 * assert_file_content - Verifies that a file's entire content exactly matches an expected string.
 *
 * @path: Filesystem path to check.
 * @expected: Expected null-terminated string.
 */
static void
assert_file_content(const char *path, const char *expected)
{
	char buffer[128];
	size_t expected_length = strlen(expected);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	assert(fd >= 0);
	assert(expected_length < sizeof(buffer));
	n = read(fd, buffer, sizeof(buffer));
	assert(n == (ssize_t)expected_length);
	assert(memcmp(buffer, expected, expected_length) == 0);
	assert(read(fd, buffer, sizeof(buffer)) == 0);
	assert(close(fd) == 0);
}

/**
 * wait_for_file - Polls until a file exists on disk, with a 2-second timeout.
 *
 * @path: Filesystem path to wait for.
 */
static void
wait_for_file(const char *path)
{
	struct stat st;
	unsigned int attempt;

	for (attempt = 0; attempt < 200; attempt++) {
		if (stat(path, &st) == 0)
			return;
		usleep(10000);
	}
	assert(!"timed out waiting for collector output");
}

/**
 * wait_for_size - Polls until a file reaches an expected size in bytes.
 *
 * @path: Filesystem path to poll.
 * @expected: Expected file size in bytes.
 */
static void
wait_for_size(const char *path, off_t expected)
{
	struct stat st;
	unsigned int attempt;

	for (attempt = 0; attempt < 200; attempt++) {
		if (stat(path, &st) == 0 && st.st_size == expected)
			return;
		usleep(10000);
	}
	assert(!"timed out waiting for collector bytes");
}

/**
 * wait_for_counter - Polls until a shared metric counter reaches a minimum value.
 *
 * @counter: Pointer to atomic uint64_t counter in shared memory.
 * @minimum: Minimum threshold value.
 */
static void
wait_for_counter(const uint64_t *counter, uint64_t minimum)
{
	unsigned int attempt;

	for (attempt = 0; attempt < 300; attempt++) {
		if (fdr_metrics_load_u64(counter) >= minimum)
			return;
		usleep(10000);
	}
	assert(!"timed out waiting for collector metric");
}

/**
 * test_signal_reopen - Tests asynchronous file reopening upon receiving SIGUSR1.
 *
 * Simulates external logrotate behavior:
 * 1. Worker opens `output.log` and writes initial events.
 * 2. Test harness renames `output.log` to `output.moved` and sends `SIGUSR1` to child.
 * 3. Harvest loop intercepts signal and creates a new `output.log` file.
 * 4. Verifies subsequent events are written to the new file descriptor without data loss.
 */
static void
test_signal_reopen(void)
{
	char tempdir[] = "/tmp/fdr-harvest-reopen-XXXXXX";
	char tracepipe[FDR_PATH_MAX];
	char logfile[FDR_PATH_MAX];
	char moved[FDR_PATH_MAX];
	struct fdr_instance instance;
	struct fdr_item item;
	const char before[] = "before reopen\n";
	const char after[] = "after reopen\n";
	pid_t child;
	int writer;
	int status;

	assert(mkdtemp(tempdir) != NULL);
	assert(fdr_join_path(tracepipe, sizeof(tracepipe), tempdir,
	    "trace_pipe") == 0);
	assert(fdr_join_path(logfile, sizeof(logfile), tempdir, "output.log") == 0);
	assert(fdr_join_path(moved, sizeof(moved), tempdir, "output.moved") == 0);
	assert(mkfifo(tracepipe, 0600) == 0);

	fdr_instance_init(&instance);
	assert(fdr_copy_field(instance.iname, sizeof(instance.iname),
	    "fdr-unit-reopen") == 0);
	assert(fdr_copy_field(instance.dname, sizeof(instance.dname), tempdir) == 0);
	instance.minfree = 0;
	memset(&item, 0, sizeof(item));
	item.type = FDR_ITEM_SAVETO;
	assert(fdr_copy_field(item.target, sizeof(item.target), logfile) == 0);

	child = fork();
	assert(child >= 0);
	if (child == 0)
		_exit(fdr_harvest_run(&instance, &item) == 0 ? 0 : 1);

	writer = open(tracepipe, O_WRONLY | O_CLOEXEC);
	assert(writer >= 0);
	assert(fdr_write_all(writer, before, strlen(before)) == 0);
	wait_for_size(logfile, (off_t)strlen(before));

	/* Simulate log rotation rename */
	assert(rename(logfile, moved) == 0);
	assert(kill(child, SIGUSR1) == 0);
	wait_for_file(logfile);

	assert(fdr_write_all(writer, after, strlen(after)) == 0);
	wait_for_size(logfile, (off_t)strlen(after));
	assert(close(writer) == 0);
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	assert_file_content(moved, before);
	assert_file_content(logfile, after);

	assert(unlink(tracepipe) == 0);
	assert(unlink(logfile) == 0);
	assert(unlink(moved) == 0);
	assert(rmdir(tempdir) == 0);
}

/**
 * test_rotation_failure_recovery - Tests rate-limited retries and recovery during rotation errors.
 *
 * Sequence:
 * 1. Simulates filesystem permission error by making parent directory read-only (`chmod 0500`).
 * 2. Feeds trace data to trigger rotation; verifies rotation failure counter increments,
 *    incoming bytes are dropped, and readiness degrades (`healthy = 0`).
 * 3. Verifies consecutive writes within the 1-second backoff window are dropped without spamming rename(2).
 * 4. Restores directory write permissions (`chmod 0700`) and waits past the backoff timer.
 * 5. Feeds new data; verifies rotation succeeds and logs are backed up properly.
 */
static void
test_rotation_failure_recovery(void)
{
	char tempdir[] = "/tmp/fdr-harvest-rotation-XXXXXX";
	char tracepipe[FDR_PATH_MAX];
	char logfile[FDR_PATH_MAX];
	char backup[FDR_PATH_MAX];
	struct fdr_instance instance;
	struct fdr_item item;
	const char full[] = "generation-000\n";
	const char dropped[] = "generation-001\n";
	const char suppressed[] = "generation-002\n";
	const char recovered[] = "generation-003\n";
	uint64_t dropped_before;
	uint64_t failures_before;
	uint64_t rotations_before;
	pid_t child;
	int writer;
	int status;

	assert(strlen(full) == strlen(dropped));
	assert(strlen(full) == strlen(recovered));
	assert(mkdtemp(tempdir) != NULL);
	assert(fdr_join_path(tracepipe, sizeof(tracepipe), tempdir,
	    "trace_pipe") == 0);
	assert(fdr_join_path(logfile, sizeof(logfile), tempdir, "output.log") == 0);
	assert(snprintf(backup, sizeof(backup), "%s.1", logfile) <
	    (int)sizeof(backup));
	assert(mkfifo(tracepipe, 0600) == 0);
	write_trace(logfile, full);

	fdr_instance_init(&instance);
	assert(fdr_copy_field(instance.iname, sizeof(instance.iname),
	    "fdr-unit-rotation-recovery") == 0);
	assert(fdr_copy_field(instance.dname, sizeof(instance.dname), tempdir) == 0);
	instance.minfree = 0;
	instance.maxsize = strlen(full);
	memset(&item, 0, sizeof(item));
	item.type = FDR_ITEM_SAVETO;
	assert(fdr_copy_field(item.target, sizeof(item.target), logfile) == 0);

	dropped_before = fdr_metrics_load_u64(&fdr.metrics->bytes_dropped);
	failures_before = fdr_metrics_load_u64(&fdr.metrics->rotation_failures);
	rotations_before = fdr_metrics_load_u64(&fdr.metrics->rotations);
	fdr_metrics_store_int(&fdr.metrics->healthy, 1);

	/* Make directory read-only to force rename(2) to fail with EACCES */
	assert(chmod(tempdir, 0500) == 0);

	child = fork();
	assert(child >= 0);
	if (child == 0)
		_exit(fdr_harvest_run(&instance, &item) == 0 ? 0 : 1);

	writer = open(tracepipe, O_WRONLY | O_CLOEXEC);
	assert(writer >= 0);
	assert(fdr_write_all(writer, dropped, strlen(dropped)) == 0);
	wait_for_counter(&fdr.metrics->rotation_failures, failures_before + 1);
	wait_for_counter(&fdr.metrics->bytes_dropped,
	    dropped_before + strlen(dropped));
	assert(fdr_metrics_load_int(&fdr.metrics->healthy) == 0);

	/* Verify immediate write within backoff window is dropped without re-attempting rename */
	assert(fdr_write_all(writer, suppressed, strlen(suppressed)) == 0);
	wait_for_counter(&fdr.metrics->bytes_dropped,
	    dropped_before + strlen(dropped) + strlen(suppressed));
	assert(fdr_metrics_load_u64(&fdr.metrics->rotation_failures) ==
	    failures_before + 1);

	/* Restore permissions and wait past 1-second backoff timer */
	assert(chmod(tempdir, 0700) == 0);
	usleep(1100000);

	assert(fdr_write_all(writer, recovered, strlen(recovered)) == 0);
	wait_for_counter(&fdr.metrics->rotations, rotations_before + 1);
	wait_for_size(logfile, (off_t)strlen(recovered));
	assert(close(writer) == 0);
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	assert_file_content(backup, full);
	assert_file_content(logfile, recovered);
	assert(fdr_metrics_load_u64(&fdr.metrics->rotation_failures) ==
	    failures_before + 1);
	assert(fdr_metrics_load_u64(&fdr.metrics->bytes_dropped) ==
	    dropped_before + strlen(dropped) + strlen(suppressed));

	assert(unlink(tracepipe) == 0);
	assert(unlink(logfile) == 0);
	assert(unlink(backup) == 0);
	assert(rmdir(tempdir) == 0);
}

int
main(void)
{
	char tempdir[] = "/tmp/fdr-harvest-XXXXXX";
	char tracepipe[FDR_PATH_MAX];
	char logfile[FDR_PATH_MAX];
	char backup[FDR_PATH_MAX];
	struct fdr_instance instance;
	struct fdr_item item;
	struct stat st;
	const char payload[] = "trace event\n";

	assert(mkdtemp(tempdir) != NULL);
	assert(fdr_join_path(tracepipe, sizeof(tracepipe), tempdir,
	    "trace_pipe") == 0);
	assert(fdr_join_path(logfile, sizeof(logfile), tempdir, "output.log") == 0);
	assert(snprintf(backup, sizeof(backup), "%s.1", logfile) <
	    (int)sizeof(backup));
	write_trace(tracepipe, payload);

	fdr_instance_init(&instance);
	assert(fdr_copy_field(instance.iname, sizeof(instance.iname),
	    "fdr-unit-no-logrotate") == 0);
	assert(fdr_copy_field(instance.dname, sizeof(instance.dname), tempdir) == 0);
	instance.minfree = 100;
	memset(&item, 0, sizeof(item));
	item.type = FDR_ITEM_SAVETO;
	assert(fdr_copy_field(item.target, sizeof(item.target), logfile) == 0);

	/* Initialize shared memory metrics */
	fdr_metrics_init();

	/* --- Test Suite 1: Disk Space Protection (minfree 100% forces drop) --- */
	assert(fdr_harvest_run(&instance, &item) == 0);
	assert(stat(logfile, &st) == 0 && st.st_size == 0);
	assert(fdr_metrics_load_u64(&fdr.metrics->bytes_dropped) ==
	    strlen(payload));
	assert(fdr_metrics_load_int(&fdr.metrics->healthy) == 0);
	fdr_metrics_store_int(&fdr.metrics->healthy, 1);

	/* --- Test Suite 2: Normal Trace Persistence --- */
	write_trace(tracepipe, payload);
	instance.minfree = 0; /* Direct test-only setting: disable the guard */
	assert(fdr_harvest_run(&instance, &item) == 0);
	assert(stat(logfile, &st) == 0 &&
	    st.st_size == (off_t)strlen(payload));
	assert(fdr_metrics_load_u64(&fdr.metrics->bytes_written) ==
	    strlen(payload));

	/* --- Test Suite 3: Bounded Size Log Rotation --- */
	write_trace(tracepipe, payload);
	instance.maxsize = strlen(payload);
	assert(fdr_harvest_run(&instance, &item) == 0);
	assert(stat(logfile, &st) == 0 &&
	    st.st_size == (off_t)strlen(payload));
	assert(stat(backup, &st) == 0 &&
	    st.st_size == (off_t)strlen(payload));
	assert(fdr_metrics_load_u64(&fdr.metrics->rotations) == 1);

	/* --- Test Suite 4 & 5: Asynchronous Reopening & Failure Recovery --- */
	test_signal_reopen();
	test_rotation_failure_recovery();

	fdr_metrics_destroy();

	assert(unlink(tracepipe) == 0);
	assert(unlink(logfile) == 0);
	assert(unlink(backup) == 0);
	assert(rmdir(tempdir) == 0);
	puts("harvest tests passed");
	return 0;
}

