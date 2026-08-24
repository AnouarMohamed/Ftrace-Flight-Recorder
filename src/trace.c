/*
 * trace.c - ftrace instance and probe management
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

static void
fdr_add_saturating(uint64_t *total, uint64_t value)
{
	if (UINT64_MAX - *total < value)
		*total = UINT64_MAX;
	else
		*total += value;
}

static int
fdr_trace_parse_stats(const char *path, uint64_t *overruns,
    uint64_t *dropped, uint64_t *commit_overruns)
{
	char line[256];
	FILE *fp;
	int found_overruns = 0;
	int found_dropped = 0;
	int found_commit = 0;

	fp = fopen(path, "re");
	if (fp == NULL)
		return -1;
	while (fgets(line, sizeof(line), fp) != NULL) {
		uint64_t value;

		if (sscanf(line, "overrun: %" SCNu64, &value) == 1) {
			*overruns = value;
			found_overruns = 1;
		} else if (sscanf(line, "dropped events: %" SCNu64,
		    &value) == 1) {
			*dropped = value;
			found_dropped = 1;
		} else if (sscanf(line, "commit overrun: %" SCNu64,
		    &value) == 1) {
			*commit_overruns = value;
			found_commit = 1;
		}
	}
	if (ferror(fp) || fclose(fp) != 0)
		return -1;
	return found_overruns && found_dropped && found_commit ? 0 : -1;
}

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

static int
fdr_trace_read_loss(const struct fdr_instance *insp, uint64_t *overruns,
    uint64_t *dropped, uint64_t *commit_overruns)
{
	char per_cpu[FDR_PATH_MAX];
	DIR *dir;
	struct dirent *entry;
	int cpus = 0;
	int read_error = 0;

	*overruns = 0;
	*dropped = 0;
	*commit_overruns = 0;
	if (fdr_join_path(per_cpu, sizeof(per_cpu), insp->dname, "per_cpu") != 0)
		return -1;
	dir = opendir(per_cpu);
	if (dir == NULL)
		return -1;
	for (;;) {
		char cpu_dir[FDR_PATH_MAX];
		char stats[FDR_PATH_MAX];
		uint64_t cpu_overruns = 0;
		uint64_t cpu_dropped = 0;
		uint64_t cpu_commit = 0;

		errno = 0;
		entry = readdir(dir);
		if (entry == NULL) {
			read_error = errno;
			break;
		}
		if (!fdr_cpu_dir_name(entry->d_name))
			continue;
		if (fdr_join_path(cpu_dir, sizeof(cpu_dir), per_cpu,
		    entry->d_name) != 0 ||
		    fdr_join_path(stats, sizeof(stats), cpu_dir, "stats") != 0 ||
		    fdr_trace_parse_stats(stats, &cpu_overruns, &cpu_dropped,
		    &cpu_commit) != 0)
			continue;
		fdr_add_saturating(overruns, cpu_overruns);
		fdr_add_saturating(dropped, cpu_dropped);
		fdr_add_saturating(commit_overruns, cpu_commit);
		cpus++;
	}
	if (read_error != 0) {
		(void)closedir(dir);
		return -1;
	}
	if (closedir(dir) != 0)
		return -1;
	return cpus > 0 ? 0 : -1;
}

static uint64_t
fdr_trace_counter_delta(uint64_t current, uint64_t *previous)
{
	uint64_t delta = current >= *previous ? current - *previous : current;

	*previous = current;
	return delta;
}

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
	if (overrun_delta == 0 && dropped_delta == 0 && commit_delta == 0)
		return 0;
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

void
fdr_trace_sample_all_loss(void)
{
	struct fdr_instance *insp;

	for (insp = fdr.instances; insp != NULL; insp = insp->next)
		(void)fdr_trace_sample_loss(insp);
}

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

int
fdr_trace_create_instance(struct fdr_instance *insp,
    const struct fdr_item *item)
{
	char path[FDR_PATH_MAX];
	char value[32];

	(void)item;
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

static int
fdr_probe_path(char *path, size_t pathsz, const struct fdr_instance *insp,
    const char *target, const char *control)
{
	int n = snprintf(path, pathsz, "%s/events/%s/%s",
	    insp->dname, target, control);

	return n < 0 || (size_t)n >= pathsz ? -1 : 0;
}

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
	if (strcmp(event, "all") == 0)
		event[-1] = '\0';

	/* A filter must be valid before the event is enabled. */
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
