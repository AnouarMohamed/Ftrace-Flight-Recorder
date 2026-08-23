/*
 * trace.c - ftrace instance and probe management
 */

#include "fdr.h"

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
