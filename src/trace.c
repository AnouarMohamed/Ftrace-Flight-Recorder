/*
 * trace.c - ftrace instance and probe management
 */

#include "fdr.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void
fdr_trace_create_instance(struct fdr_instance *insp, struct fdr_item *item)
{
	char path[FDR_PATH_MAX], value[FDR_BUFSIZE];
	int fd, len;

	fprintf(stderr, "creating: %s\n", insp->dname);
	(void)rmdir(insp->dname);
	errno = 0;
	if (mkdir(insp->dname, 0700) && errno != EEXIST) {
		perror(insp->dname);
		exit(FDR_EC_MKDIR);
	}

	if (insp->bufsize == 0)
		return;

	fprintf(stderr, "%s: bufsize %lu\n", item->target, insp->bufsize);
	snprintf(path, sizeof(path), "%.*s/buffer_size_kb",
	    (int)(sizeof(path) - 16), insp->dname);

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror(path);
		return;
	}

	snprintf(value, sizeof(value), "%lu", insp->bufsize);
	len = (int)strlen(value);
	if (write(fd, value, (size_t)len) == -1)
		perror(path);
	close(fd);
}

void
fdr_trace_load_module(struct fdr_item *item)
{
	char cmdline[FDR_BUFSIZE];

	snprintf(cmdline, sizeof(cmdline), "modprobe %s", item->target);
	if (system(cmdline) != 0) {
		perror(cmdline);
		exit(FDR_EC_SYSTEM);
	}
}

void
fdr_trace_set_probe(struct fdr_instance *insp, struct fdr_item *item)
{
	char path[FDR_PATH_MAX];
	char target[FDR_BUFSIZE];
	char *slash, *value, *action;
	int fd, len;

	fdr_copy_field(target, sizeof(target), item->target);

	slash = strchr(target, '/');
	if (slash == NULL) {
		fprintf(stderr, "missing slash on line %d in %s\n",
		    item->line, item->fpath);
		exit(FDR_EC_SYNTAX);
	}
	slash++;
	if (strncmp(slash, "all", 3) == 0)
		*--slash = '\0';

	snprintf(path, sizeof(path), "%s/%s/events/%s/enable",
	    fdr.inst_dir, insp->iname, target);

	if (item->type == FDR_ITEM_ENABLE) {
		value = "1";
		action = "enable";
	} else {
		value = "0";
		action = "disable";
	}

	if (fdr.verbose > 1)
		fprintf(stderr, "%s: %s\n", action, path);

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		if (errno == ENOENT) {
			fprintf(stderr, "%s: no such probe\n", item->target);
			return;
		}
		perror(path);
		exit(FDR_EC_OPEN);
	}

	if (write(fd, value, 1) != 1) {
		perror("write");
		close(fd);
		exit(FDR_EC_WRITE1);
	}
	close(fd);

	if (item->optarg[0] == '\0')
		return;

	snprintf(path, sizeof(path), "%s/%s/events/%s/filter",
	    fdr.inst_dir, insp->iname, target);
	fprintf(stderr, "applying filter '%s' to %s\n", item->optarg, path);

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror(path);
		return;
	}
	len = (int)strlen(item->optarg);
	if (write(fd, item->optarg, (size_t)len) != len)
		perror(path);
	close(fd);
}
