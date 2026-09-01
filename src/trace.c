/*
 * trace.c - Linux ftrace instance control, probe configuration, and kernel loss sampling
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Overview:
 * This module manages the Linux kernel tracefs interface:
 * 1. Instance Creation & Sizing: Allocates isolated tracefs subdirectories under
 *    `/sys/kernel/tracing/instances/<name>` and writes per-CPU ring-buffer sizes
 *    to `buffer_size_kb`.
 * 2. Probe Configuration: Configures event filters before writing to `enable`
 *    nodes to prevent unfiltered event bursts into the ring buffer.
 * 3. Module Loading: Invokes `modprobe` via safe fork/exec with `PR_SET_PDEATHSIG`
 *    without shell interpolation.
 * 4. Forensic Loss Sampling: Periodically parses `/per_cpu/cpu[0-9]+/stats` across all
 *    CPUs to detect ring-buffer overwrites (`overrun`), dropped events, and
 *    nested interrupt losses (`commit overrun`). Immediately degrades readiness
 *    (`fdr_ready 0`) upon detecting loss.
 */

#include "fdr.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

/** Refresh cached CPU topology once per minute at the five-second sample rate. */
#define FDR_TRACE_TOPOLOGY_REFRESH_SAMPLES 12U

/**
 * fdr_add_saturating - Performs saturating addition on 64-bit unsigned integers.
 *
 * Prevents integer wraparound if kernel loss counters reach astronomical values.
 * If *total + value would overflow UINT64_MAX, clamps *total to UINT64_MAX.
 *
 * @total: Pointer to accumulator uint64_t.
 * @value: Value to add.
 */
static void
fdr_add_saturating(uint64_t *total, uint64_t value)
{
	if (UINT64_MAX - *total < value)
		*total = UINT64_MAX;
	else
		*total += value;
}

/**
 * fdr_trace_parse_counter - Parses an unsigned 64-bit integer following a label.
 *
 * Validates that `line` starts with `label`, skips whitespace, parses ASCII digits
 * with overflow detection, and verifies there are no trailing garbage characters.
 *
 * @line: Pointer to line buffer.
 * @length: Length of line buffer in bytes.
 * @label: Expected label prefix (e.g., "overrun:", "dropped events:").
 * @value: Pointer to uint64_t to store parsed integer.
 * Return: 1 if matched and parsed, 0 if label does not match, -1 on syntax/overflow error.
 */
static int
fdr_trace_parse_counter(const char *line, size_t length, const char *label,
    uint64_t *value)
{
	size_t label_length = strlen(label);
	size_t cursor;
	uint64_t parsed = 0;

	if (length < label_length || memcmp(line, label, label_length) != 0)
		return 0;

	cursor = label_length;
	while (cursor < length && (line[cursor] == ' ' || line[cursor] == '\t'))
		cursor++;

	if (cursor == length || !isdigit((unsigned char)line[cursor]))
		return -1;

	while (cursor < length && isdigit((unsigned char)line[cursor])) {
		unsigned int digit = (unsigned int)(line[cursor] - '0');

		/* Overflow check before multiply by 10 */
		if (parsed > (UINT64_MAX - digit) / 10)
			return -1;
		parsed = parsed * 10 + digit;
		cursor++;
	}

	while (cursor < length && (line[cursor] == ' ' || line[cursor] == '\t' ||
	    line[cursor] == '\r'))
		cursor++;

	if (cursor != length)
		return -1;

	*value = parsed;
	return 1;
}

/**
 * fdr_trace_parse_stats_line - Matches a line from per_cpu stats against known loss labels.
 *
 * Checks for "overrun:", "dropped events:", and "commit overrun:".
 *
 * @line: Line content buffer.
 * @length: Byte length of line.
 * @overruns: Pointer to store parsed overrun count.
 * @dropped: Pointer to store parsed dropped event count.
 * @commit_overruns: Pointer to store parsed commit overrun count.
 * @found_overruns: Flag updated to 1 when overrun label is encountered.
 * @found_dropped: Flag updated to 1 when dropped events label is encountered.
 * @found_commit: Flag updated to 1 when commit overrun label is encountered.
 */
static void
fdr_trace_parse_stats_line(const char *line, size_t length,
    uint64_t *overruns, uint64_t *dropped, uint64_t *commit_overruns,
    int *found_overruns, int *found_dropped, int *found_commit)
{
	int parsed;

	parsed = fdr_trace_parse_counter(line, length, "overrun:", overruns);
	if (parsed != 0) {
		if (parsed > 0)
			*found_overruns = 1;
		return;
	}
	parsed = fdr_trace_parse_counter(line, length, "dropped events:", dropped);
	if (parsed != 0) {
		if (parsed > 0)
			*found_dropped = 1;
		return;
	}
	parsed = fdr_trace_parse_counter(line, length, "commit overrun:",
	    commit_overruns);
	if (parsed > 0)
		*found_commit = 1;
}

/**
 * fdr_trace_parse_stats - Streams and parses a kernel per_cpu/cpuN/stats file.
 *
 * Uses a fixed 4 KiB stack buffer and sliding window newline splitting to avoid
 * dynamic allocations and handle arbitrary kernel buffer sizes.
 *
 * @path: Full path to /sys/kernel/tracing/instances/<name>/per_cpu/cpuN/stats.
 * @overruns: Pointer to receive parsed overrun count.
 * @dropped: Pointer to receive parsed dropped event count.
 * @commit_overruns: Pointer to receive parsed commit overrun count.
 * Return: 0 if all 3 loss counters were successfully found and parsed, -1 on error.
 */
static int
fdr_trace_parse_stats(const char *path, uint64_t *overruns,
    uint64_t *dropped, uint64_t *commit_overruns)
{
	char buffer[4096];
	size_t used = 0;
	int discarding = 0;
	int fd;
	int found_overruns = 0;
	int found_dropped = 0;
	int found_commit = 0;
	int rc = -1;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	for (;;) {
		ssize_t n;
		size_t start = 0;

		n = read(fd, buffer + used, sizeof(buffer) - used);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			goto out;
		}
		used += (size_t)n;

		/* Process complete lines split by '\n' */
		while (start < used) {
			char *newline = memchr(buffer + start, '\n', used - start);

			if (newline == NULL)
				break;
			if (!discarding)
				fdr_trace_parse_stats_line(buffer + start,
				    (size_t)(newline - (buffer + start)), overruns,
				    dropped, commit_overruns, &found_overruns,
				    &found_dropped, &found_commit);
			discarding = 0;
			start = (size_t)(newline - buffer) + 1;
		}

		/* Shift remaining unparsed bytes to front of buffer */
		if (start != 0) {
			memmove(buffer, buffer + start, used - start);
			used -= start;
		}

		/* EOF reached */
		if (n == 0) {
			if (used != 0 && !discarding)
				fdr_trace_parse_stats_line(buffer, used, overruns,
				    dropped, commit_overruns, &found_overruns,
				    &found_dropped, &found_commit);
			break;
		}

		/* Discard oversized lines that exceed buffer */
		if (used == sizeof(buffer)) {
			used = 0;
			discarding = 1;
		}
	}
	rc = found_overruns && found_dropped && found_commit ? 0 : -1;
out:
	if (close(fd) != 0)
		rc = -1;
	return rc;
}

/**
 * fdr_cpu_dir_name - Identifies directory entries matching CPU folder format ("cpu[0-9]+").
 *
 * @name: Directory entry name (e.g. "cpu0", "cpu12", "buffer_size_kb").
 * Return: 1 if format matches cpuN, 0 otherwise.
 */
static int
fdr_cpu_dir_name(const char *name)
{
	const unsigned char *cursor;

	if (strncmp(name, "cpu", 3) != 0 || name[3] == '\0')
		return 0;
	for (cursor = (const unsigned char *)name + 3; *cursor != '\0'; cursor++) {
		if (!isdigit(*cursor))
			return 0;
	}
	return 1;
}

/**
 * fdr_trace_cache_path - Appends one stats path to a temporary topology cache.
 *
 * @pathsp: In/out pointer to the path array.
 * @countp: In/out pointer to the populated path count.
 * @capacityp: In/out pointer to the allocated array capacity.
 * @path: Stats path to copy.
 * Return: 0 on success, or -1 on allocation failure.
 */
static int
fdr_trace_cache_path(char ***pathsp, size_t *countp, size_t *capacityp,
    const char *path)
{
	char **resized;
	char *copy;
	size_t capacity;

	if (*countp == *capacityp) {
		capacity = *capacityp == 0 ? 8 : *capacityp * 2;
		if (capacity < *capacityp || capacity > SIZE_MAX / sizeof(**pathsp))
			return -1;
		resized = realloc(*pathsp, capacity * sizeof(**pathsp));
		if (resized == NULL)
			return -1;
		*pathsp = resized;
		*capacityp = capacity;
	}

	copy = strdup(path);
	if (copy == NULL)
		return -1;
	(*pathsp)[(*countp)++] = copy;
	return 0;
}

/**
 * fdr_trace_refresh_loss_cache - Rediscovers readable per-CPU stats paths.
 *
 * Builds a replacement cache before releasing the current one, so an allocation
 * or directory-read failure cannot leave partially initialized state behind.
 *
 * @insp: Target tracefs instance.
 * Return: 0 when at least one CPU path was cached, or -1 on failure.
 */
static int
fdr_trace_refresh_loss_cache(struct fdr_instance *insp)
{
	char per_cpu[FDR_PATH_MAX];
	char **paths = NULL;
	size_t count = 0;
	size_t capacity = 0;
	DIR *dir;
	struct dirent *entry;
	int rc = -1;

	if (fdr_join_path(per_cpu, sizeof(per_cpu), insp->dname, "per_cpu") != 0)
		return -1;
	dir = opendir(per_cpu);
	if (dir == NULL)
		return -1;

	for (;;) {
		char cpu_dir[FDR_PATH_MAX];
		char stats[FDR_PATH_MAX];

		errno = 0;
		entry = readdir(dir);
		if (entry == NULL) {
			if (errno == 0 && count > 0)
				rc = 0;
			break;
		}
		if (!fdr_cpu_dir_name(entry->d_name))
			continue;
		if (fdr_join_path(cpu_dir, sizeof(cpu_dir), per_cpu,
		    entry->d_name) != 0 ||
		    fdr_join_path(stats, sizeof(stats), cpu_dir, "stats") != 0 ||
		    access(stats, R_OK) != 0)
			continue;
		if (fdr_trace_cache_path(&paths, &count, &capacity, stats) != 0)
			break;
	}
	if (closedir(dir) != 0)
		rc = -1;

	if (rc == 0) {
		fdr_trace_reset_loss_cache(insp);
		insp->trace_stats_paths = paths;
		insp->trace_stats_path_count = count;
		insp->trace_stats_online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
		return 0;
	}

	while (count > 0)
		free(paths[--count]);
	free(paths);
	return -1;
}

/**
 * fdr_trace_read_loss - Reads and sums loss metrics across all online CPUs for an instance.
 *
 * Reuses cached `cpuN/stats` paths and sums overruns, dropped events, and commit
 * overruns using saturating addition. The topology is refreshed once per minute,
 * when the online CPU count changes, and when a cached path disappears. This
 * covers CPU hotplug without rescanning the directory on every five-second sample.
 *
 * @insp: Target tracefs instance.
 * @overruns: Pointer to store aggregated overrun count across all CPUs.
 * @dropped: Pointer to store aggregated dropped events count.
 * @commit_overruns: Pointer to store aggregated commit overruns count.
 * Return: 0 on success (at least one CPU parsed), or -1 on failure.
 */
static int
fdr_trace_read_loss(struct fdr_instance *insp, uint64_t *overruns,
    uint64_t *dropped, uint64_t *commit_overruns)
{
	size_t index;
	int refreshed = 0;
	long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);

	if (insp->trace_stats_path_count == 0 ||
	    insp->trace_stats_samples >= FDR_TRACE_TOPOLOGY_REFRESH_SAMPLES ||
	    (online_cpus > 0 && insp->trace_stats_online_cpus > 0 &&
	    online_cpus != insp->trace_stats_online_cpus)) {
		if (fdr_trace_refresh_loss_cache(insp) != 0)
			return -1;
		refreshed = 1;
	}

retry:

	*overruns = 0;
	*dropped = 0;
	*commit_overruns = 0;

	for (index = 0; index < insp->trace_stats_path_count; index++) {
		uint64_t cpu_overruns = 0;
		uint64_t cpu_dropped = 0;
		uint64_t cpu_commit = 0;

		if (fdr_trace_parse_stats(insp->trace_stats_paths[index],
		    &cpu_overruns, &cpu_dropped, &cpu_commit) != 0) {
			if (refreshed || fdr_trace_refresh_loss_cache(insp) != 0)
				return -1;
			refreshed = 1;
			goto retry;
		}

		fdr_add_saturating(overruns, cpu_overruns);
		fdr_add_saturating(dropped, cpu_dropped);
		fdr_add_saturating(commit_overruns, cpu_commit);
	}

	insp->trace_stats_samples++;
	return 0;
}

/**
 * fdr_trace_counter_delta - Computes the non-negative change in a loss counter.
 *
 * Updates `*previous` to `current` and returns the delta. Handles counter resets.
 *
 * @current: Newly sampled cumulative counter value from kernel.
 * @previous: Pointer to previously recorded counter value.
 * Return: Delta change since last sample.
 */
static uint64_t
fdr_trace_counter_delta(uint64_t current, uint64_t *previous)
{
	uint64_t delta = current >= *previous ? current - *previous : current;

	*previous = current;
	return delta;
}

/**
 * fdr_trace_sample_loss - Samples loss counters for an instance and updates metrics.
 *
 * If any new overruns, dropped events, or commit overruns are detected:
 * - Atomically increments Prometheus counters in shared memory.
 * - Degrades daemon readiness state (healthy = 0).
 * - Emits a warning log message on the first observed loss.
 *
 * @insp: Target tracefs instance.
 * Return: 1 if loss was detected, 0 if healthy/no new loss, -1 on read error.
 */
int
fdr_trace_sample_loss(struct fdr_instance *insp)
{
	uint64_t overruns;
	uint64_t dropped;
	uint64_t commit_overruns;
	uint64_t overrun_delta;
	uint64_t dropped_delta;
	uint64_t commit_delta;

	if (fdr.metrics == NULL ||
	    fdr_trace_read_loss(insp, &overruns, &dropped,
	    &commit_overruns) != 0)
		return -1;

	overrun_delta = fdr_trace_counter_delta(overruns,
	    &insp->last_trace_overruns);
	dropped_delta = fdr_trace_counter_delta(dropped,
	    &insp->last_trace_dropped);
	commit_delta = fdr_trace_counter_delta(commit_overruns,
	    &insp->last_trace_commit_overruns);

	fdr_metrics_add(&fdr.metrics->trace_overruns, overrun_delta);
	fdr_metrics_add(&fdr.metrics->trace_dropped_events, dropped_delta);
	fdr_metrics_add(&fdr.metrics->trace_commit_overruns, commit_delta);

	/* If zero new loss was detected, remain in current readiness state */
	if (overrun_delta == 0 && dropped_delta == 0 && commit_delta == 0)
		return 0;

	/* Mark recorder readiness as degraded (fdr_ready 0) */
	fdr_metrics_store_int(&fdr.metrics->healthy, 0);

	if (!insp->trace_loss_reported) {
		fdr_warn("trace data loss detected for instance %s "
		    "(overruns=%" PRIu64 ", dropped=%" PRIu64
		    ", commit-overruns=%" PRIu64 ")",
		    insp->iname, overruns, dropped, commit_overruns);
		insp->trace_loss_reported = 1;
	}
	return 1;
}

/**
 * fdr_trace_sample_all_loss - Samples kernel loss across all active loaded instances.
 */
void
fdr_trace_sample_all_loss(void)
{
	struct fdr_instance *insp;

	for (insp = fdr.instances; insp != NULL; insp = insp->next)
		(void)fdr_trace_sample_loss(insp);
}

/**
 * fdr_write_control - Writes a control string to a tracefs virtual file node.
 *
 * Opens with O_WRONLY | O_CLOEXEC, writes the string using fdr_write_all, and closes.
 *
 * @path: Full path to tracefs control node (e.g. ".../events/sched/sched_switch/enable").
 * @value: String value to write (e.g. "1", "0", or filter expression).
 * Return: 0 on success, or -1 on error.
 */
static int
fdr_write_control(const char *path, const char *value)
{
	int fd;
	int rc;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		fdr_warn("cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	rc = fdr_write_all(fd, value, strlen(value));
	if (rc != 0)
		fdr_warn("cannot write %s: %s", path, strerror(errno));
	if (close(fd) != 0 && rc == 0) {
		fdr_warn("cannot close %s: %s", path, strerror(errno));
		rc = -1;
	}
	return rc;
}

/**
 * fdr_trace_create_instance - Creates the tracefs instance and configures per-CPU buffers.
 *
 * Cleans up any leftover directory from an ungraceful prior exit (rmdir), creates
 * the instance directory (`mkdir ... 0700`), and writes `buffer_size_kb` if specified.
 *
 * @insp: Target instance structure.
 * @item: Directing AST node.
 * Return: 0 on success, or -1 on error.
 */
int
fdr_trace_create_instance(struct fdr_instance *insp,
    const struct fdr_item *item)
{
	char path[FDR_PATH_MAX];
	char value[32];

	(void)item;
	/* Remove stale instance if present from a previous crash */
	if (rmdir(insp->dname) != 0 && errno != ENOENT) {
		fdr_warn("cannot remove stale trace instance %s: %s",
		    insp->dname, strerror(errno));
		return -1;
	}
	if (mkdir(insp->dname, 0700) != 0) {
		fdr_warn("cannot create trace instance %s: %s",
		    insp->dname, strerror(errno));
		return -1;
	}
	fdr_log("info", "created trace instance %s", insp->iname);

	if (insp->bufsize_kb == 0)
		return 0;

	if (fdr_join_path(path, sizeof(path), insp->dname, "buffer_size_kb") != 0) {
		fdr_warn("trace buffer path is too long for %s", insp->iname);
		return -1;
	}
	(void)snprintf(value, sizeof(value), "%" PRIu64, insp->bufsize_kb);
	if (fdr_write_control(path, value) != 0)
		return -1;

	fdr_log("info", "set instance %s buffer to %" PRIu64 " KiB per CPU",
	    insp->iname, insp->bufsize_kb);
	return 0;
}

/**
 * fdr_trace_load_module - Executes modprobe to load a required kernel module.
 *
 * Forks a child process, sets PR_SET_PDEATHSIG to SIGTERM so the child terminates
 * if the parent dies, and invokes `modprobe -- <target>` directly via execlp
 * without shell interpolation.
 *
 * @item: Directive AST node containing module name in item->target.
 * Return: 0 if modprobe succeeded (exit status 0), or -1 on failure.
 */
int
fdr_trace_load_module(const struct fdr_item *item)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		fdr_warn("cannot fork modprobe: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		pid_t parent = getppid();

		/* Ensure child dies if supervisor terminates prematurely */
		if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || parent == 1 ||
		    getppid() != parent)
			_exit(FDR_EC_SYSTEM);
		execlp("modprobe", "modprobe", "--", item->target, (char *)NULL);
		fdr_log("error", "cannot execute modprobe: %s", strerror(errno));
		_exit(FDR_EC_EXEC);
	}
	do {
		pid_t waited = waitpid(pid, &status, 0);

		if (waited == pid)
			break;
		if (waited < 0 && errno != EINTR) {
			fdr_warn("cannot wait for modprobe: %s", strerror(errno));
			return -1;
		}
	} while (1);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fdr_warn("modprobe %s failed", item->target);
		return -1;
	}
	return 0;
}

/**
 * fdr_probe_path - Constructs the full path to a tracepoint control node.
 *
 * Formats: `<instance_dname>/events/<subsystem>/<event>/<control>`
 *
 * @path: Destination path buffer.
 * @pathsz: Size of destination path buffer.
 * @insp: Target instance.
 * @target: Event specifier (e.g. "sched/sched_switch" or "sched/all").
 * @control: Control filename ("enable" or "filter").
 * Return: 0 on success, or -1 on path truncation.
 */
static int
fdr_probe_path(char *path, size_t pathsz, const struct fdr_instance *insp,
    const char *target, const char *control)
{
	int n = snprintf(path, pathsz, "%s/events/%s/%s",
	    insp->dname, target, control);

	return n < 0 || (size_t)n >= pathsz ? -1 : 0;
}

/**
 * fdr_trace_set_probe - Configures a tracepoint filter and enable/disable state.
 *
 * Sequencing invariant:
 * - If an event filter is specified (item->optarg), the filter is written to
 *   the event's `filter` node BEFORE the event is enabled. This prevents
 *   unfiltered events from flooding the trace buffer during configuration.
 * - Writes "1" (enable) or "0" (disable) to the event's `enable` node.
 *
 * Upon failure, increments `probe_failures` counter and sets readiness to 0.
 *
 * @insp: Target tracefs instance.
 * @item: Directive AST node specifying target probe and optional filter.
 * Return: 0 on success, or -1 on probe configuration failure.
 */
int
fdr_trace_set_probe(struct fdr_instance *insp, const struct fdr_item *item)
{
	char path[FDR_PATH_MAX];
	char target[FDR_NAME_MAX * 2];
	char *event;
	const char *value;

	if (fdr_copy_field(target, sizeof(target), item->target) != 0)
		return -1;

	event = strchr(target, '/');
	if (event == NULL)
		return -1;
	event++;
	/* Handle subsystem-wide probes (e.g. "sched/all" -> "events/sched/enable") */
	if (strcmp(event, "all") == 0)
		event[-1] = '\0';

	/* Write filter before enabling probe to prevent unfiltered trace bursts */
	if (item->type == FDR_ITEM_ENABLE && item->optarg[0] != '\0') {
		if (fdr_probe_path(path, sizeof(path), insp, target, "filter") != 0 ||
		    fdr_write_control(path, item->optarg) != 0)
			goto failed;
	}

	value = item->type == FDR_ITEM_ENABLE ? "1" : "0";
	if (fdr_probe_path(path, sizeof(path), insp, target, "enable") != 0 ||
	    fdr_write_control(path, value) != 0)
		goto failed;

	if (fdr.verbose)
		fdr_log("info", "%s probe %s",
		    item->type == FDR_ITEM_ENABLE ? "enabled" : "disabled",
		    item->target);
	return 0;

failed:
	if (fdr.metrics != NULL) {
		fdr_metrics_add(&fdr.metrics->probe_failures, 1);
		fdr_metrics_store_int(&fdr.metrics->healthy, 0);
	}
	fdr_warn("failed to configure probe %s (%s:%d)", item->target,
	    item->fpath, item->line);
	return -1;
}
