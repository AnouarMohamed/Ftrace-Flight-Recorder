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
 * 5. Failure Recovery: Simulates rotation failure (conflicting backup directory), tests retry
 *    rate-limiting and automatic recovery once the conflict is removed.
 * 6. Free-Space Scheduling: Verifies byte-budget and monotonic-time boundaries.
 */

#include "fdr.h"

#include <assert.h>
#include <fcntl.h>
#include <glob.h>
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
 * write_executable - Writes a temporary executable helper script.
 *
 * @path: Destination path.
 * @content: Script contents.
 */
static void
write_executable(const char *path, const char *content)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0700);

	assert(fd >= 0);
	assert(fdr_write_all(fd, content, strlen(content)) == 0);
	assert(close(fd) == 0);
	assert(chmod(path, 0700) == 0);
}

/**
 * find_glob - Finds the first matching path, optionally accepting partial output.
 *
 * @pattern: Glob pattern to evaluate.
 * @allow_partial: Whether names ending in `.partial` are acceptable.
 * @match: Output buffer for the selected path.
 * @matchsz: Size of the output buffer.
 * Return: 1 when a matching path was found, 0 otherwise.
 */
static int
find_glob(const char *pattern, int allow_partial, char *match, size_t matchsz)
{
	glob_t matches;
	size_t i;
	int found = 0;

	memset(&matches, 0, sizeof(matches));
	if (glob(pattern, 0, NULL, &matches) != 0) {
		globfree(&matches);
		return 0;
	}
	for (i = 0; i < matches.gl_pathc; i++) {
		size_t length = strlen(matches.gl_pathv[i]);

		if (!allow_partial && length >= 8 &&
		    strcmp(matches.gl_pathv[i] + length - 8, ".partial") == 0)
			continue;
		assert(strlen(matches.gl_pathv[i]) < matchsz);
		strcpy(match, matches.gl_pathv[i]);
		found = 1;
		break;
	}
	globfree(&matches);
	return found;
}

/**
 * wait_for_glob - Polls until a matching path appears.
 *
 * @pattern: Glob pattern to evaluate.
 * @allow_partial: Whether names ending in `.partial` are acceptable.
 * @match: Output buffer for the selected path.
 * @matchsz: Size of the output buffer.
 */
static void
wait_for_glob(const char *pattern, int allow_partial, char *match,
    size_t matchsz)
{
	unsigned int attempt;

	for (attempt = 0; attempt < 300; attempt++) {
		if (find_glob(pattern, allow_partial, match, matchsz))
			return;
		usleep(10000);
	}
	assert(!"timed out waiting for retention output");
}

/**
 * remove_glob - Removes every file matching a glob pattern.
 *
 * @pattern: Glob pattern to remove.
 */
static void
remove_glob(const char *pattern)
{
	glob_t matches;
	size_t i;

	memset(&matches, 0, sizeof(matches));
	if (glob(pattern, 0, NULL, &matches) != 0) {
		globfree(&matches);
		return;
	}
	for (i = 0; i < matches.gl_pathc; i++)
		(void)unlink(matches.gl_pathv[i]);
	globfree(&matches);
}

/**
 * test_async_retention_success - Verifies collection continues while retention is delayed.
 *
 * The helper blocks until the test releases it. A second trace block must reach
 * the new active file while the helper is still blocked, proving that rotation
 * does not wait for retention work. Releasing the helper must publish the sealed
 * generation only after it is complete.
 */
static void
test_async_retention_success(void)
{
	char tempdir[] = "/tmp/fdr-harvest-retention-XXXXXX";
	char tracepipe[FDR_PATH_MAX];
	char logfile[FDR_PATH_MAX];
	char helper[FDR_PATH_MAX];
	char started[FDR_PATH_MAX];
	char release[FDR_PATH_MAX];
	char glob_pattern[FDR_PATH_MAX];
	char retained[FDR_PATH_MAX];
	char script[2048];
	struct fdr_instance instance;
	struct fdr_item item;
	const char first[] = "generation-one\n";
	const char second[] = "generation-two\n";
	const char third[] = "x\n";
	uint64_t failures_before;
	pid_t child;
	int writer;
	int status;
	int fd;

	assert(mkdtemp(tempdir) != NULL);
	assert(fdr_join_path(tracepipe, sizeof(tracepipe), tempdir,
	    "trace_pipe") == 0);
	assert(fdr_join_path(logfile, sizeof(logfile), tempdir, "output.log") == 0);
	assert(fdr_join_path(helper, sizeof(helper), tempdir, "retain.sh") == 0);
	assert(fdr_join_path(started, sizeof(started), tempdir,
	    "helper.started") == 0);
	assert(fdr_join_path(release, sizeof(release), tempdir,
	    "helper.release") == 0);
	assert(snprintf(glob_pattern, sizeof(glob_pattern),
	    "%s.retained.*", logfile) < (int)sizeof(glob_pattern));
	assert(mkfifo(tracepipe, 0600) == 0);
	assert(snprintf(script, sizeof(script),
	    "#!/bin/sh\n"
	    "printf started > '%s'\n"
	    "while [ ! -e '%s' ]; do sleep 0.01; done\n"
	    "cat \"$1\" > \"$2\"\n",
	    started, release) < (int)sizeof(script));
	write_executable(helper, script);

	fdr_instance_init(&instance);
	assert(fdr_copy_field(instance.iname, sizeof(instance.iname),
	    "fdr-unit-retention") == 0);
	assert(fdr_copy_field(instance.dname, sizeof(instance.dname), tempdir) == 0);
	assert(fdr_copy_field(instance.retention, sizeof(instance.retention),
	    helper) == 0);
	instance.minfree = 0;
	instance.maxsize = strlen(first) + strlen(second) - 1;
	memset(&item, 0, sizeof(item));
	item.type = FDR_ITEM_SAVETO;
	assert(fdr_copy_field(item.target, sizeof(item.target), logfile) == 0);
	failures_before = fdr_metrics_load_u64(&fdr.metrics->retention_failures);

	child = fork();
	assert(child >= 0);
	if (child == 0)
		_exit(fdr_harvest_run(&instance, &item) == 0 ? 0 : 1);

	writer = open(tracepipe, O_WRONLY | O_CLOEXEC);
	assert(writer >= 0);
	assert(fdr_write_all(writer, first, strlen(first)) == 0);
	wait_for_size(logfile, (off_t)strlen(first));
	assert(fdr_write_all(writer, second, strlen(second)) == 0);
	wait_for_file(started);
	wait_for_size(logfile, (off_t)strlen(second));

	/* The helper is still blocked, but the collector must keep draining. */
	assert(fdr_write_all(writer, third, strlen(third)) == 0);
	wait_for_size(logfile, (off_t)(strlen(second) + strlen(third)));

	fd = open(release, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	assert(fd >= 0);
	assert(close(fd) == 0);
	wait_for_glob(glob_pattern, 0, retained, sizeof(retained));
	assert_file_content(retained, first);
	assert(fdr_metrics_load_u64(&fdr.metrics->retention_failures) ==
	    failures_before);

	assert(close(writer) == 0);
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	assert(unlink(tracepipe) == 0);
	assert(unlink(logfile) == 0);
	assert(unlink(helper) == 0);
	assert(unlink(started) == 0);
	assert(unlink(release) == 0);
	{
		char cleanup[FDR_PATH_MAX];

		assert(snprintf(cleanup, sizeof(cleanup), "%s.*", logfile) <
		    (int)sizeof(cleanup));
		remove_glob(cleanup);
	}
	assert(rmdir(tempdir) == 0);
}

/**
 * test_async_retention_failure - Verifies helper failures stay visible and non-destructive.
 *
 * The helper writes a partial output and exits non-zero. FDR must increment the
 * retention failure counter, leave the sealed generation and partial output
 * distinguishable from completed output, and keep collecting new trace data.
 */
static void
test_async_retention_failure(void)
{
	char tempdir[] = "/tmp/fdr-harvest-retainfail-XXXXXX";
	char tracepipe[FDR_PATH_MAX];
	char logfile[FDR_PATH_MAX];
	char helper[FDR_PATH_MAX];
	char pending_glob[FDR_PATH_MAX];
	char partial_glob[FDR_PATH_MAX];
	char pending[FDR_PATH_MAX];
	char partial[FDR_PATH_MAX];
	struct fdr_instance instance;
	struct fdr_item item;
	const char first[] = "generation-one\n";
	const char second[] = "generation-two\n";
	const char third[] = "x\n";
	uint64_t failures_before;
	uint64_t rotations_before;
	pid_t child;
	int writer;
	int status;

	assert(mkdtemp(tempdir) != NULL);
	assert(fdr_join_path(tracepipe, sizeof(tracepipe), tempdir,
	    "trace_pipe") == 0);
	assert(fdr_join_path(logfile, sizeof(logfile), tempdir, "output.log") == 0);
	assert(fdr_join_path(helper, sizeof(helper), tempdir, "retain.sh") == 0);
	assert(snprintf(pending_glob, sizeof(pending_glob), "%s.pending.*",
	    logfile) < (int)sizeof(pending_glob));
	assert(snprintf(partial_glob, sizeof(partial_glob),
	    "%s.retained.*.partial", logfile) < (int)sizeof(partial_glob));
	assert(mkfifo(tracepipe, 0600) == 0);
	write_executable(helper,
	    "#!/bin/sh\n"
	    "printf partial > \"$2\"\n"
	    "exit 1\n");

	fdr_instance_init(&instance);
	assert(fdr_copy_field(instance.iname, sizeof(instance.iname),
	    "fdr-unit-retainfail") == 0);
	assert(fdr_copy_field(instance.dname, sizeof(instance.dname), tempdir) == 0);
	assert(fdr_copy_field(instance.retention, sizeof(instance.retention),
	    helper) == 0);
	instance.minfree = 0;
	instance.maxsize = strlen(first) + strlen(second) - 1;
	memset(&item, 0, sizeof(item));
	item.type = FDR_ITEM_SAVETO;
	assert(fdr_copy_field(item.target, sizeof(item.target), logfile) == 0);
	failures_before = fdr_metrics_load_u64(&fdr.metrics->retention_failures);
	rotations_before = fdr_metrics_load_u64(&fdr.metrics->rotations);

	child = fork();
	assert(child >= 0);
	if (child == 0)
		_exit(fdr_harvest_run(&instance, &item) == 0 ? 0 : 1);

	writer = open(tracepipe, O_WRONLY | O_CLOEXEC);
	assert(writer >= 0);
	assert(fdr_write_all(writer, first, strlen(first)) == 0);
	wait_for_size(logfile, (off_t)strlen(first));
	assert(fdr_write_all(writer, second, strlen(second)) == 0);
	wait_for_counter(&fdr.metrics->retention_failures, failures_before + 1);
	wait_for_size(logfile, (off_t)strlen(second));
	assert(fdr_metrics_load_u64(&fdr.metrics->rotations) >=
	    rotations_before + 1);

	wait_for_glob(pending_glob, 1, pending, sizeof(pending));
	wait_for_glob(partial_glob, 1, partial, sizeof(partial));
	assert_file_content(partial, "partial");
	assert_file_content(pending, first);

	/* Collection must continue after a retention failure. */
	assert(fdr_write_all(writer, third, strlen(third)) == 0);
	wait_for_size(logfile, (off_t)(strlen(second) + strlen(third)));

	assert(close(writer) == 0);
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	assert(unlink(tracepipe) == 0);
	assert(unlink(logfile) == 0);
	assert(unlink(helper) == 0);
	assert(unlink(pending) == 0);
	assert(unlink(partial) == 0);
	assert(rmdir(tempdir) == 0);
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
 * 1. Creates a directory at the backup path so rename(2) fails consistently for every UID.
 * 2. Feeds trace data to trigger rotation; verifies rotation failure counter increments,
 *    incoming bytes are dropped, and readiness degrades (`healthy = 0`).
 * 3. Verifies consecutive writes within the 1-second backoff window are dropped without spamming rename(2).
 * 4. Removes the conflicting directory and waits past the backoff timer.
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
	assert(mkdir(backup, 0700) == 0);

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

	/* Remove the rename conflict and wait past the 1-second backoff timer */
	assert(rmdir(backup) == 0);
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

/**
 * test_trace_buffer_metric - Verifies configured buffer accounting across CPUs.
 *
 * Builds a temporary instance list with explicit and default buffer sizes,
 * recomputes the shared gauge, and checks the expected per-CPU total.
 */
static void
test_trace_buffer_metric(void)
{
	struct fdr_instance explicit_small;
	struct fdr_instance explicit_large;
	struct fdr_instance kernel_default;
	long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	uint64_t expected;

	assert(online_cpus > 0);
	fdr_instance_init(&explicit_small);
	fdr_instance_init(&explicit_large);
	fdr_instance_init(&kernel_default);
	explicit_small.bufsize_kb = 1;
	explicit_large.bufsize_kb = 64;

	fdr.instances = NULL;
	fdr.instance_count = 0;
	fdr_instance_append(&explicit_small);
	fdr_instance_append(&explicit_large);
	fdr_instance_append(&kernel_default);
	fdr_metrics_update_trace_buffer();

	expected = (UINT64_C(1) + UINT64_C(64)) * 1024U *
	    (uint64_t)online_cpus;
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_buffer_bytes) == expected);

	fdr.instances = NULL;
	fdr.instance_count = 0;
}

/**
 * test_space_check_schedule - Verifies byte and monotonic scheduling boundaries.
 *
 * Uses fixed timestamps so the five-second boundary and clock rollback can be
 * checked deterministically without sleeping.
 */
static void
test_space_check_schedule(void)
{
	struct timespec last = { .tv_sec = 100, .tv_nsec = 900000000 };
	struct timespec now;

	assert(fdr_space_check_due(FDR_SPACE_CHECK_BYTES, &last, &last) == 1);
	assert(fdr_space_check_due(FDR_SPACE_CHECK_BYTES - 1, &last, &last) == 0);

	now = last;
	now.tv_sec += FDR_SPACE_CHECK_SECONDS - 1;
	now.tv_nsec = 999999999;
	assert(fdr_space_check_due(0, &last, &now) == 0);

	now = last;
	now.tv_sec += FDR_SPACE_CHECK_SECONDS;
	assert(fdr_space_check_due(0, &last, &now) == 1);

	now.tv_nsec = 899999999;
	assert(fdr_space_check_due(0, &last, &now) == 0);

	now = last;
	now.tv_sec -= 1;
	assert(fdr_space_check_due(0, &last, &now) == 0);

	now = last;
	now.tv_sec += FDR_SPACE_CHECK_SECONDS + 1;
	assert(fdr_space_check_due(0, &last, &now) == 1);
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
	test_trace_buffer_metric();
	test_space_check_schedule();

	/* --- Test Suite 1: Disk Space Protection (minfree 100% forces drop) --- */
	assert(fdr_harvest_run(&instance, &item) == 0);
	assert(stat(logfile, &st) == 0 && st.st_size == 0);
	assert(fdr_metrics_load_u64(&fdr.metrics->bytes_dropped) ==
	    strlen(payload));
	assert(fdr_metrics_load_u64(&fdr.metrics->read_calls) == 1);
	assert(fdr_metrics_load_u64(&fdr.metrics->read_bytes) ==
	    strlen(payload));
	assert(fdr_metrics_load_u64(&fdr.metrics->space_checks) == 1);
	assert(fdr_metrics_load_u64(&fdr.metrics->write_calls) == 0);
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
	assert(fdr_metrics_load_u64(&fdr.metrics->read_calls) == 2);
	assert(fdr_metrics_load_u64(&fdr.metrics->read_bytes) ==
	    2U * strlen(payload));
	assert(fdr_metrics_load_u64(&fdr.metrics->space_checks) == 2);
	assert(fdr_metrics_load_u64(&fdr.metrics->write_calls) == 1);

	/* --- Test Suite 3: Bounded Size Log Rotation --- */
	write_trace(tracepipe, payload);
	instance.maxsize = strlen(payload);
	assert(fdr_harvest_run(&instance, &item) == 0);
	assert(stat(logfile, &st) == 0 &&
	    st.st_size == (off_t)strlen(payload));
	assert(stat(backup, &st) == 0 &&
	    st.st_size == (off_t)strlen(payload));
	assert(fdr_metrics_load_u64(&fdr.metrics->rotations) == 1);
	assert(fdr_metrics_load_u64(&fdr.metrics->read_calls) == 3);
	assert(fdr_metrics_load_u64(&fdr.metrics->write_calls) == 2);
	assert(fdr_metrics_load_u64(&fdr.metrics->rotation_duration_ns) > 0);

	/* --- Test Suite 4 & 5: Asynchronous Reopening & Failure Recovery --- */
	test_signal_reopen();
	test_rotation_failure_recovery();
	test_async_retention_success();
	test_async_retention_failure();

	fdr_metrics_destroy();

	assert(unlink(tracepipe) == 0);
	assert(unlink(logfile) == 0);
	assert(unlink(backup) == 0);
	assert(rmdir(tempdir) == 0);
	puts("harvest tests passed");
	return 0;
}
