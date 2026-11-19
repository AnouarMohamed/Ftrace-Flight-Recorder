/*
 * harvest.c - Kernel trace_pipe data-plane harvester, filesystem protection, and bounded rotation
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Overview:
 * This module implements the high-throughput, continuous capture data-plane loop
 * executed inside each child worker process:
 * 1. Stream Harvesting: Continuously drains the kernel character device
 *    `/sys/kernel/tracing/instances/<name>/trace_pipe` in blocking read loops.
 * 2. Secure File Persistence: Writes mode-0600 regular files without following
 *    symlinks (`O_NOFOLLOW | O_CLOEXEC`), validating file descriptors with `fstat`.
 * 3. Bounded File Rotation: Tracks in-memory file size (`current_size`), triggering
 *    rotation before writes exceed `maxsize`. Delegates to `/etc/logrotate.d/<name>`
 *    if present, otherwise executes an atomic fallback rename to `<path>.1`.
 * 4. Filesystem Free-Space Protection: Periodically queries `fstatvfs` against the
 *    configured `minfree` percentage threshold. Discards incoming trace blocks and
 *    increments `fdr_bytes_dropped_total` rather than risking host filesystem exhaustion.
 * 5. Signal Handling: Intercepts `SIGHUP` and `SIGUSR1` to cleanly reopen log files
 *    after external log rotation without losing records.
 */

#include "fdr.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/** Minimum read buffer allocation (8 KiB) to amortize syscall overhead. */
#ifndef FDR_HARVEST_BUFFER_MIN
#define FDR_HARVEST_BUFFER_MIN (8U * 1024U)
#endif

/** Maximum read buffer allocation ceiling (1 MiB). */
#define FDR_HARVEST_BUFFER_MAX (1024U * 1024U)

/** Backoff duration in seconds before retrying failed log rotation. */
#define FDR_ROTATION_RETRY_SECONDS 1

/** Maximum number of concurrently running retention helpers per worker. */
#define FDR_MAX_RETENTION_HELPERS 8

/** State for one supervised asynchronous retention helper. */
struct fdr_retention_child {
	pid_t pid;                    /**< Helper process ID, or 0 when the slot is free. */
	char source[FDR_PATH_MAX];    /**< Sealed generation handed to the helper. */
	char partial[FDR_PATH_MAX];   /**< Helper-written output awaiting publication. */
	char final[FDR_PATH_MAX];     /**< Published retained generation name. */
};

/** Per-worker asynchronous retention queue. */
struct fdr_retention_state {
	struct fdr_retention_child children[FDR_MAX_RETENTION_HELPERS];
	uint64_t sequence;            /**< Generation sequence used for unique names. */
};

/** Set when a supervised retention helper exits. */
static volatile sig_atomic_t fdr_retention_sigchld;

/**
 * fdr_monotonic_ns - Reads the monotonic clock as nanoseconds.
 *
 * @return Monotonic nanoseconds, or 0 when the clock cannot be read.
 */
static uint64_t
fdr_monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/**
 * fdr_sighup_worker - Signal handler for worker processes receiving SIGHUP or SIGUSR1.
 *
 * Sets the async-signal-safe flag `fdr.got_sighup` to 1, notifying the harvest loop
 * to reopen the destination log file on the next read iteration.
 *
 * @signo: Signal number received.
 */
static void
fdr_sighup_worker(int signo)
{
	(void)signo;
	fdr.got_sighup = 1;
}

/**
 * fdr_open_log - Securely opens or creates the capture output log file.
 *
 * Security & Integrity Invariants:
 * - Flags: O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW.
 * - Permissions: Explicit mode 0600 (read/write by owner only).
 * - Target Verification: Calls fstat(2) on the opened descriptor and verifies S_ISREG
 *   to guarantee the target is a regular file and prevent symlink/device hijacks.
 *
 * @path: Absolute filesystem path to the log file.
 * @sizep: Optional pointer to uint64_t to receive the initial file size from fstat.
 * Return: Open file descriptor >= 0 on success, or -1 on error.
 */
static int
fdr_open_log(const char *path, uint64_t *sizep)
{
	int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
	int fd;
	struct stat st;

#ifdef O_NOFOLLOW
	/* Refuse to follow symlinks to protect host security */
	flags |= O_NOFOLLOW;
#endif
	fd = open(path, flags, 0600);
	if (fd < 0) {
		fdr_warn("cannot open log %s: %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
		fdr_warn("log target %s is not a regular file", path);
		close(fd);
		errno = EINVAL;
		return -1;
	}
	if (sizep != NULL)
		*sizep = (uint64_t)st.st_size;
	return fd;
}

/**
 * fdr_run_logrotate - Executes logrotate if an instance policy is installed.
 *
 * Checks for `/etc/logrotate.d/<instance_name>`. If present and regular, forks and
 * executes `/usr/sbin/logrotate -f <confpath>` with `PR_SET_PDEATHSIG` to prevent zombies.
 *
 * @insp: Target instance structure.
 * Return: 1 if logrotate ran and succeeded, 0 if no config found, -1 if logrotate failed.
 */
static int
fdr_run_logrotate(const struct fdr_instance *insp)
{
	struct stat st;
	char confpath[FDR_PATH_MAX];
	pid_t pid;
	int status;

	if (snprintf(confpath, sizeof(confpath), "/etc/logrotate.d/%s",
	    insp->iname) >= (int)sizeof(confpath))
		return 0;
	if (stat(confpath, &st) != 0 || !S_ISREG(st.st_mode))
		return 0;

	pid = fork();
	if (pid < 0) {
		fdr_warn("cannot fork logrotate: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		pid_t parent = getppid();

		/* Terminate child if supervisor dies */
		if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || parent == 1 ||
		    getppid() != parent)
			_exit(FDR_EC_SYSTEM);
		execl("/usr/sbin/logrotate", "logrotate", "-f", confpath,
		    (char *)NULL);
		fdr_log("error", "cannot execute logrotate: %s", strerror(errno));
		_exit(FDR_EC_EXEC);
	}
	do {
		pid_t waited = waitpid(pid, &status, 0);

		if (waited == pid)
			break;
		if (waited < 0 && errno != EINTR) {
			fdr_warn("cannot wait for logrotate: %s", strerror(errno));
			return -1;
		}
	} while (1);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fdr_warn("logrotate failed for instance %s", insp->iname);
		return -1;
	}
	return 1;
}

/**
 * fdr_rotate_logs - Rotates a full log file when maximum bounded size is reached.
 *
 * Tries external logrotate first via `fdr_run_logrotate`. If no logrotate configuration
 * exists, performs an atomic filesystem rename from `<path>` to `<path>.1`.
 * Increments `fdr.metrics->rotations` on success.
 *
 * @insp: Target instance structure.
 * @path: Path to active output file.
 * Return: 1 on successful rotation, 0 if nothing to rotate (ENOENT), -1 on failure.
 */
static int
fdr_rotate_logs(const struct fdr_instance *insp, const char *path)
{
	char backup[FDR_PATH_MAX];
	int result;

	result = fdr_run_logrotate(insp);
	if (result > 0)
		goto rotated;

	if (snprintf(backup, sizeof(backup), "%s.1", path) >=
	    (int)sizeof(backup)) {
		fdr_warn("rotation path is too long for %s", path);
		return -1;
	}
	if (rename(path, backup) != 0) {
		if (errno == ENOENT)
			return 0;
		fdr_warn("cannot rotate %s to %s: %s", path, backup,
		    strerror(errno));
		return -1;
	}

rotated:
	if (fdr.metrics != NULL)
		fdr_metrics_add(&fdr.metrics->rotations, 1);
	fdr_log("info", "rotated log for instance %s", insp->iname);
	return 1;
}

/**
 * fdr_reopen_log - Closes old file descriptor and reopens destination log file.
 *
 * Used after rotation or after receiving SIGHUP/SIGUSR1. Updates `sizep` with
 * the newly opened file's size.
 *
 * @oldfd: Existing open file descriptor to close.
 * @path: Path to destination log file.
 * @sizep: Pointer to uint64_t to store new file size.
 * Return: New valid file descriptor >= 0 on success, or -1 on error.
 */
static int
fdr_reopen_log(int oldfd, const char *path, uint64_t *sizep)
{
	int newfd = fdr_open_log(path, sizep);

	if (newfd < 0)
		return -1;
	if (close(oldfd) != 0)
		fdr_warn("cannot close rotated log %s: %s", path, strerror(errno));
	return newfd;
}

/**
 * fdr_retention_sigchld_handler - Wakes the collector when a retention helper exits.
 *
 * @signo: Signal number received.
 */
static void
fdr_retention_sigchld_handler(int signo)
{
	(void)signo;
	fdr_retention_sigchld = 1;
}

/**
 * fdr_path_entry_exists - Checks whether any directory entry exists at a path.
 *
 * Uses lstat so broken symlinks are still treated as occupied names.
 *
 * @path: Path to inspect.
 * Return: 1 when an entry exists, 0 when it does not.
 */
static int
fdr_path_entry_exists(const char *path)
{
	struct stat st;

	if (lstat(path, &st) == 0)
		return 1;
	return errno != ENOENT;
}

/**
 * fdr_retention_paths - Builds unique pending, partial, and retained names.
 *
 * @path: Active capture path.
 * @sequence: Monotonic generation sequence.
 * @pending: Output buffer for the sealed generation name.
 * @partial: Output buffer for the helper output name.
 * @final: Output buffer for the published retained generation name.
 * Return: 0 on success, -1 when a name is too long.
 */
static int
fdr_retention_paths(const char *path, uint64_t sequence, char *pending,
    char *partial, char *final)
{
	int n;

	n = snprintf(pending, FDR_PATH_MAX, "%s.pending.%ld.%" PRIu64,
	    path, (long)getpid(), sequence);
	if (n < 0 || n >= FDR_PATH_MAX)
		return -1;
	n = snprintf(partial, FDR_PATH_MAX, "%s.retained.%ld.%" PRIu64
	    ".partial", path, (long)getpid(), sequence);
	if (n < 0 || n >= FDR_PATH_MAX)
		return -1;
	n = snprintf(final, FDR_PATH_MAX, "%s.retained.%ld.%" PRIu64,
	    path, (long)getpid(), sequence);
	if (n < 0 || n >= FDR_PATH_MAX)
		return -1;
	return 0;
}

/**
 * fdr_reap_retention - Reaps finished helpers and publishes only complete output.
 *
 * A helper writes to the partial name. FDR verifies that the helper exited
 * successfully and produced a regular file before atomically renaming it to the
 * retained name. Failed helpers leave the sealed generation and partial output
 * in place, increment a counter, and never make partial output look complete.
 *
 * @state: Retention queue to reap.
 */
static void
fdr_reap_retention(struct fdr_retention_state *state)
{
	int i;

	for (i = 0; i < FDR_MAX_RETENTION_HELPERS; i++) {
		struct fdr_retention_child *child = &state->children[i];
		struct stat st;
		pid_t waited;
		int status;
		int complete;

		if (child->pid <= 0)
			continue;
		waited = waitpid(child->pid, &status, WNOHANG);
		if (waited == 0)
			continue;
		if (waited < 0) {
			if (errno == EINTR)
				continue;
			if (errno != ECHILD)
				fdr_warn("cannot reap retention helper %d: %s",
				    (int)child->pid, strerror(errno));
			child->pid = 0;
			continue;
		}

		complete = WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
		    lstat(child->partial, &st) == 0 && S_ISREG(st.st_mode);
		if (complete && rename(child->partial, child->final) != 0) {
			fdr_warn("cannot publish retained generation %s: %s",
			    child->final, strerror(errno));
			complete = 0;
		}
		if (complete) {
			if (unlink(child->source) != 0 && errno != ENOENT)
				fdr_warn("cannot remove sealed generation %s: %s",
				    child->source, strerror(errno));
			fdr_log("info", "retention completed for %s",
			    child->final);
		} else {
			if (fdr.metrics != NULL)
				fdr_metrics_add(
				    &fdr.metrics->retention_failures, 1);
			fdr_warn("retention helper failed for %s; sealed generation remains at %s",
			    child->partial, child->source);
		}
		child->pid = 0;
	}
}

/**
 * fdr_spawn_retention - Starts one supervised retention helper.
 *
 * The helper receives the sealed source, the partial output path, and the
 * active capture path as arguments. It must write a complete retained artifact
 * to the partial path and exit 0.
 *
 * @state: Retention queue receiving the new helper.
 * @insp: Instance providing the helper executable.
 * @source: Sealed generation path.
 * @partial: Helper output path.
 * @final: Published retained generation path.
 * @active: New active capture path.
 * Return: 0 when the helper was started, -1 on fork or queue failure.
 */
static int
fdr_spawn_retention(struct fdr_retention_state *state,
    const struct fdr_instance *insp, const char *source,
    const char *partial, const char *final, const char *active)
{
	struct fdr_retention_child *child = NULL;
	pid_t pid;
	int slot;

	fdr_reap_retention(state);
	for (slot = 0; slot < FDR_MAX_RETENTION_HELPERS; slot++) {
		if (state->children[slot].pid == 0) {
			child = &state->children[slot];
			break;
		}
	}
	if (child == NULL) {
		fdr_warn("too many retention helpers for instance %s",
		    insp->iname);
		return -1;
	}
	if (fdr_copy_field(child->source, sizeof(child->source), source) != 0 ||
	    fdr_copy_field(child->partial, sizeof(child->partial), partial) != 0 ||
	    fdr_copy_field(child->final, sizeof(child->final), final) != 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		fdr_warn("cannot fork retention helper: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		pid_t parent = getppid();

		if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || parent == 1 ||
		    getppid() != parent)
			_exit(FDR_EC_SYSTEM);
		execl(insp->retention, insp->retention, source, partial, active,
		    (char *)NULL);
		fdr_log("error", "cannot execute retention helper %s: %s",
		    insp->retention, strerror(errno));
		_exit(FDR_EC_EXEC);
	}

	child->pid = pid;
	return 0;
}

/**
 * fdr_rotate_with_retention - Seals a generation and resumes capture immediately.
 *
 * The active file is renamed atomically, a new mode-0600 destination is opened
 * and validated, and only then is the retention helper started. Collection
 * never waits for helper work such as compression.
 *
 * @state: Retention queue.
 * @insp: Instance providing the helper executable.
 * @path: Active capture path.
 * @writefd: In/out active file descriptor.
 * @sizep: In/out current active file size.
 * Return: 1 on rotation, 0 when nothing exists to rotate, -1 on failure.
 */
static int
fdr_rotate_with_retention(struct fdr_retention_state *state,
    const struct fdr_instance *insp, const char *path, int *writefd,
    uint64_t *sizep)
{
	char pending[FDR_PATH_MAX];
	char partial[FDR_PATH_MAX];
	char final[FDR_PATH_MAX];
	char fallback[FDR_PATH_MAX];
	int attempt;
	int newfd;
	int named = 0;

	for (attempt = 0; attempt < 1000; attempt++) {
		state->sequence++;
		if (fdr_retention_paths(path, state->sequence, pending, partial,
		    final) != 0) {
			fdr_warn("retention path is too long for %s", path);
			return -1;
		}
		if (!fdr_path_entry_exists(pending) &&
		    !fdr_path_entry_exists(partial) &&
		    !fdr_path_entry_exists(final)) {
			named = 1;
			break;
		}
	}
	if (!named) {
		fdr_warn("cannot allocate a retention generation for %s", path);
		return -1;
	}

	if (rename(path, pending) != 0) {
		if (errno == ENOENT) {
			newfd = fdr_reopen_log(*writefd, path, sizep);
			if (newfd < 0)
				return -1;
			*writefd = newfd;
			fdr_warn("active capture %s was missing during rotation; reopened",
			    path);
			return 0;
		}
		fdr_warn("cannot seal %s as %s: %s", path, pending,
		    strerror(errno));
		return -1;
	}

	newfd = fdr_reopen_log(*writefd, path, sizep);
	if (newfd < 0) {
		if (rename(pending, path) != 0)
			fdr_warn("cannot restore sealed generation %s: %s",
			    pending, strerror(errno));
		return -1;
	}
	*writefd = newfd;

	if (fdr_spawn_retention(state, insp, pending, partial, final,
	    path) != 0) {
		if (snprintf(fallback, sizeof(fallback), "%s.1", path) <
		    (int)sizeof(fallback) && rename(pending, fallback) == 0) {
			fdr_warn("retention helper unavailable for %s; preserved fallback generation %s",
			    path, fallback);
		} else {
			fdr_warn("retention helper unavailable and fallback failed for %s",
			    path);
		}
		if (fdr.metrics != NULL)
			fdr_metrics_add(&fdr.metrics->retention_failures, 1);
	} else {
		fdr_log("info", "rotated log for instance %s; retention queued",
		    insp->iname);
	}

	if (fdr.metrics != NULL)
		fdr_metrics_add(&fdr.metrics->rotations, 1);
	return 1;
}

/**
 * fdr_rotate_and_reopen - Chooses the configured rotation path and reopens capture.
 *
 * Instances with a retention helper use the internal seal/reopen fast path.
 * Other instances retain the legacy logrotate/fallback behavior for
 * compatibility.
 *
 * @state: Retention queue.
 * @insp: Target instance.
 * @path: Active capture path.
 * @writefd: In/out active file descriptor.
 * @sizep: In/out active file size.
 * Return: 1 on rotation, 0 when nothing exists to rotate, -1 on failure.
 */
static int
fdr_rotate_and_reopen(struct fdr_retention_state *state,
    const struct fdr_instance *insp, const char *path, int *writefd,
    uint64_t *sizep)
{
	int newfd;
	int result;

	if (insp->retention[0] != '\0')
		return fdr_rotate_with_retention(state, insp, path, writefd,
		    sizep);

	result = fdr_rotate_logs(insp, path);
	if (result <= 0)
		return result;
	newfd = fdr_reopen_log(*writefd, path, sizep);
	if (newfd < 0)
		return -1;
	*writefd = newfd;
	return 1;
}

/**
 * fdr_space_available - Inspects available filesystem blocks via fstatvfs(3).
 *
 * Calculates available free block percentage: `(f_bavail * 100) / f_blocks`.
 *
 * @fd: File descriptor residing on target filesystem.
 * @minfree: Percentage threshold (1-100).
 * Return: 1 if available space > minfree, 0 if at or below minfree, -1 on statvfs error.
 */
static int
fdr_space_available(int fd, int minfree)
{
	struct statvfs vfs;
	long double pctfree;

	if (fstatvfs(fd, &vfs) != 0) {
		fdr_warn("cannot inspect log filesystem: %s", strerror(errno));
		return -1;
	}
	if (vfs.f_blocks == 0)
		return 1;
	pctfree = ((long double)vfs.f_bavail * 100.0L) /
	    (long double)vfs.f_blocks;
	return pctfree > (long double)minfree;
}

/**
 * fdr_rotation_retry_ready - Checks if monotonic backoff timer for rotation retry has elapsed.
 *
 * @retry_after: Target monotonic timespec expiration.
 * Return: 1 if ready to retry rotation, 0 if still in backoff window.
 */
static int
fdr_rotation_retry_ready(const struct timespec *retry_after)
{
	struct timespec now;

	if (retry_after->tv_sec == 0 && retry_after->tv_nsec == 0)
		return 1;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 1;
	return now.tv_sec > retry_after->tv_sec ||
	    (now.tv_sec == retry_after->tv_sec &&
	    now.tv_nsec >= retry_after->tv_nsec);
}

/**
 * fdr_schedule_rotation_retry - Sets next rotation retry timestamp 1 second in future.
 *
 * @retry_after: Pointer to timespec structure to populate.
 */
static void
fdr_schedule_rotation_retry(struct timespec *retry_after)
{
	if (clock_gettime(CLOCK_MONOTONIC, retry_after) != 0) {
		retry_after->tv_sec = 0;
		retry_after->tv_nsec = 0;
		return;
	}
	retry_after->tv_sec += FDR_ROTATION_RETRY_SECONDS;
}

/**
 * fdr_space_check_due - Decides whether storage protection must run again.
 *
 * @bytes_since_check: Bytes read since the last completed free-space check.
 * @last_check: Monotonic time of the last completed check.
 * @now: Current monotonic time.
 * Return: 1 when the byte budget or five-second interval is reached, 0 otherwise.
 */
int
fdr_space_check_due(uint64_t bytes_since_check,
    const struct timespec *last_check, const struct timespec *now)
{
	time_t elapsed_seconds;

	if (bytes_since_check >= FDR_SPACE_CHECK_BYTES)
		return 1;
	if (last_check == NULL || now == NULL)
		return 0;
	if (now->tv_sec < last_check->tv_sec ||
	    (now->tv_sec == last_check->tv_sec &&
	    now->tv_nsec < last_check->tv_nsec))
		return 0;

	elapsed_seconds = now->tv_sec - last_check->tv_sec;
	if (elapsed_seconds > FDR_SPACE_CHECK_SECONDS)
		return 1;
	return elapsed_seconds == FDR_SPACE_CHECK_SECONDS &&
	    now->tv_nsec >= last_check->tv_nsec;
}

/**
 * fdr_space_check_run - Checks free space and updates warning state.
 *
 * @fd: Open destination file descriptor.
 * @insp: Instance providing the minfree threshold.
 * @target: Destination path used in diagnostics.
 * @space_ok: In/out availability state; set to 1 when space is safe.
 * @warned_space: In/out warning latch to avoid repeated low-space messages.
 * @last_check: In/out monotonic timestamp of the completed check.
 * Return: 0 on success, -1 when the filesystem cannot be inspected.
 */
static int
fdr_space_check_run(int fd, const struct fdr_instance *insp,
    const char *target, int *space_ok, int *warned_space,
    struct timespec *last_check)
{
	struct timespec now;
	int available;

	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
		*last_check = now;
	available = fdr_space_available(fd, insp->minfree);
	if (available < 0)
		return -1;
	if (!available && !*warned_space) {
		fdr_warn("free space is at or below %d%% for %s; dropping data",
		    insp->minfree, target);
		*warned_space = 1;
	} else if (available && *warned_space) {
		fdr_log("info", "free space recovered for %s", target);
		*warned_space = 0;
	}
	*space_ok = available;
	return 0;
}

/**
 * fdr_count_drop - Records intentionally discarded bytes in metrics and degrades readiness.
 *
 * Called when data is discarded due to minfree protection or rotation failure.
 *
 * @bytes: Number of bytes discarded.
 */
static void
fdr_count_drop(size_t bytes)
{
	if (fdr.metrics != NULL) {
		fdr_metrics_add(&fdr.metrics->bytes_dropped, (uint64_t)bytes);
		fdr_metrics_store_int(&fdr.metrics->healthy, 0);
	}
}

/**
 * fdr_harvest_run - Main data-plane loop: drains trace_pipe to disk.
 *
 * Execution flow:
 * 1. Installs SIGHUP and SIGUSR1 signal handlers for non-blocking file reopen.
 * 2. Opens `/sys/kernel/tracing/instances/<name>/trace_pipe` for blocking reading.
 * 3. Allocates read buffer sized between FDR_HARVEST_BUFFER_MIN and FDR_HARVEST_BUFFER_MAX.
 * 4. Opens destination log file with mode 0600.
 * 5. Enters infinite read/check/write loop:
 *    - Reopens log if signal was received.
 *    - Reads next block from kernel trace_pipe.
 *    - Checks free space after the earlier of 1 MiB read or five seconds,
 *      and immediately after reopen or rotation; drops data if space is low.
 *    - Checks bounded file size (`maxsize`); triggers rotation if next block exceeds limit.
 *    - Writes buffer to disk via `fdr_write_all` and updates `bytes_written` metric.
 *
 * @insp: Target instance structure.
 * @item: Directing AST node containing saveto target path.
 * Return: 0 on clean EOF, or -1 on unrecoverable I/O error.
 */
int
fdr_harvest_run(struct fdr_instance *insp, const struct fdr_item *item)
{
	char tracepipe[FDR_PATH_MAX];
	char *buffer;
	int readfd = -1;
	int writefd = -1;
	int blocksize;
	int space_ok = 1;
	uint64_t bytes_since_space_check = FDR_SPACE_CHECK_BYTES;
	uint64_t current_size = 0;
	int warned_space = 0;
	int rotation_failed = 0;
	struct fdr_retention_state retention;
	struct stat st;
	struct sigaction sa;
	struct timespec last_space_check = { 0, 0 };
	struct timespec rotation_retry_after = { 0, 0 };
	int rc = -1;

	memset(&retention, 0, sizeof(retention));

	if (fdr_join_path(tracepipe, sizeof(tracepipe), insp->dname,
	    "trace_pipe") != 0) {
		fdr_warn("trace pipe path is too long for %s", insp->iname);
		return -1;
	}

	/* Install signal handlers for external log rotation signals */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fdr_sighup_worker;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGHUP, &sa, NULL) != 0 ||
	    sigaction(SIGUSR1, &sa, NULL) != 0) {
		fdr_warn("cannot install worker reopen signal handler: %s",
		    strerror(errno));
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fdr_retention_sigchld_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGCHLD, &sa, NULL) != 0)
		fdr_warn("cannot install retention signal handler: %s",
		    strerror(errno));

	readfd = open(tracepipe, O_RDONLY | O_CLOEXEC);
	if (readfd < 0) {
		fdr_warn("cannot open %s: %s", tracepipe, strerror(errno));
		return -1;
	}
	if (fstat(readfd, &st) != 0) {
		fdr_warn("cannot stat %s: %s", tracepipe, strerror(errno));
		goto out;
	}

	/* Compute read buffer capacity */
	if (st.st_blksize >= (long)FDR_HARVEST_BUFFER_MIN &&
	    st.st_blksize <= (long)FDR_HARVEST_BUFFER_MAX)
		blocksize = (int)st.st_blksize;
	else
		blocksize = (int)FDR_HARVEST_BUFFER_MIN;

	buffer = malloc((size_t)blocksize);
	if (buffer == NULL) {
		fdr_warn("cannot allocate trace buffer");
		goto out;
	}
	writefd = fdr_open_log(item->target, &current_size);
	if (writefd < 0) {
		free(buffer);
		goto out;
	}

	fdr_log("info", "saving instance %s to %s", insp->iname, item->target);

	for (;;) {
		ssize_t n;

		/* Publish or report retention helpers that have exited. */
		if (fdr_retention_sigchld) {
			fdr_retention_sigchld = 0;
			fdr_reap_retention(&retention);
		}

		/* Check if reopen signal (SIGHUP / SIGUSR1) was caught */
		if (fdr.got_sighup) {
			int newfd;

			fdr.got_sighup = 0;
			newfd = fdr_reopen_log(writefd, item->target, &current_size);
			if (newfd < 0)
				break;
			writefd = newfd;
			fdr_log("info", "reopened %s after reopen signal", item->target);
			if (fdr_space_check_run(writefd, insp, item->target,
			    &space_ok, &warned_space, &last_space_check) != 0)
				break;
			bytes_since_space_check = 0;
		}

		/* Blocking read from kernel trace_pipe */
		n = read(readfd, buffer, (size_t)blocksize);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fdr_warn("cannot read %s: %s", tracepipe, strerror(errno));
			break;
		}
		if (n == 0) {
			rc = 0;
			break;
		}
		if (fdr.metrics != NULL) {
			fdr_metrics_add(&fdr.metrics->read_calls, 1);
			fdr_metrics_add(&fdr.metrics->read_bytes, (uint64_t)n);
		}

		/* Periodic filesystem free space inspection */
		{
			int space_due;

			space_due = bytes_since_space_check >=
			    FDR_SPACE_CHECK_BYTES;
			if (!space_due) {
				struct timespec now;

				if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
					space_due = fdr_space_check_due(
					    bytes_since_space_check,
					    &last_space_check, &now);
			}
			if (space_due) {
				if (fdr.metrics != NULL)
					fdr_metrics_add(&fdr.metrics->space_checks, 1);
				if (fdr_space_check_run(writefd, insp,
				    item->target, &space_ok, &warned_space,
				    &last_space_check) != 0)
					break;
				bytes_since_space_check = 0;
			}
		}
		if (UINT64_MAX - bytes_since_space_check < (uint64_t)n)
			bytes_since_space_check = UINT64_MAX;
		else
			bytes_since_space_check += (uint64_t)n;

		if (!space_ok) {
			fdr_count_drop((size_t)n);
			continue;
		}

		/* Bounded log file size rotation check */
		if (insp->maxsize != UINT64_MAX) {
			if ((uint64_t)n > insp->maxsize) {
				fdr_warn("trace block exceeds maximum size for %s; dropping",
				    item->target);
				fdr_count_drop((size_t)n);
				continue;
			}
			if (current_size > insp->maxsize - (uint64_t)n) {
				uint64_t rotation_started;
				uint64_t rotation_elapsed;
				int rotation_result;

				if (rotation_failed &&
				    !fdr_rotation_retry_ready(&rotation_retry_after)) {
					fdr_count_drop((size_t)n);
					continue;
				}
				rotation_started = fdr_monotonic_ns();
				rotation_result = fdr_rotate_and_reopen(&retention,
				    insp, item->target, &writefd,
				    &current_size);
				rotation_elapsed = fdr_monotonic_ns();
				if (fdr.metrics != NULL && rotation_started != 0 &&
				    rotation_elapsed >= rotation_started)
					fdr_metrics_add(&fdr.metrics->rotation_duration_ns,
					    rotation_elapsed - rotation_started);
				if (rotation_result < 0) {
					if (fdr.metrics != NULL)
						fdr_metrics_add(
						    &fdr.metrics->rotation_failures, 1);
					fdr_schedule_rotation_retry(&rotation_retry_after);
					rotation_failed = 1;
					fdr_count_drop((size_t)n);
					continue;
				}
				if (rotation_failed) {
					fdr_log("info", "rotation recovered for instance %s",
					    insp->iname);
					rotation_failed = 0;
					rotation_retry_after.tv_sec = 0;
					rotation_retry_after.tv_nsec = 0;
				}
				if (fdr_space_check_run(writefd, insp,
				    item->target, &space_ok, &warned_space,
				    &last_space_check) != 0)
					break;
				bytes_since_space_check = 0;
				if (!space_ok) {
					fdr_count_drop((size_t)n);
					continue;
				}
			}
		}

		/* Write buffer to log file */
		if (fdr.metrics != NULL)
			fdr_metrics_add(&fdr.metrics->write_calls, 1);
		if (fdr_write_all(writefd, buffer, (size_t)n) != 0) {
			fdr_warn("cannot write %s: %s", item->target, strerror(errno));
			if (fdr.metrics != NULL)
				fdr_metrics_add(&fdr.metrics->write_errors, 1);
			fdr_count_drop((size_t)n);
			break;
		}
		if (fdr.metrics != NULL)
			fdr_metrics_add(&fdr.metrics->bytes_written, (uint64_t)n);
		if (insp->maxsize != UINT64_MAX)
			current_size += (uint64_t)n;
	}

	if (rc != 0 && fdr.metrics != NULL)
		fdr_metrics_store_int(&fdr.metrics->healthy, 0);
	if (close(writefd) != 0)
		fdr_warn("cannot close %s: %s", item->target, strerror(errno));
	free(buffer);
out:
	if (readfd >= 0)
		close(readfd);
	return rc;
}

