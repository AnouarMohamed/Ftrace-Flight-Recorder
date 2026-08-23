/*
 * config.c - configuration file parsing
 */

#include "fdr.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct fdr_item *
fdr_item_append(struct fdr_item *head, struct fdr_item *item)
{
	struct fdr_item *cur;

	if (head == NULL)
		return item;

	for (cur = head; cur->next != NULL; cur = cur->next)
		;
	cur->next = item;
	return head;
}

static int
fdr_parse_line(struct fdr_instance *insp, const char *fpath, int line,
    char *linebuf)
{
	char save_buf[FDR_BUFSIZE];
	char *bp, *cp, *verbp, *targetp, *optarg, *savep;
	struct fdr_item *item;

	fdr_chomp_line(linebuf);
	fdr_copy_field(save_buf, sizeof(save_buf), linebuf);

	bp = linebuf;
	verbp = targetp = optarg = NULL;
	while ((cp = strtok_r(bp, " \t", &savep)) != NULL) {
		bp = NULL;
		if (verbp == NULL) {
			verbp = cp;
			continue;
		}
		if (targetp == NULL) {
			targetp = cp;
			continue;
		}
		if (optarg == NULL) {
			optarg = &save_buf[cp - linebuf];
			if (fdr.verbose > 1)
				fprintf(stderr, "optarg: %s\n", optarg);
			break;
		}
	}

	if (verbp == NULL || targetp == NULL)
		return 0;

	item = calloc(1, sizeof(*item));
	if (item == NULL) {
		perror("calloc");
		return -1;
	}

	fdr_copy_field(item->verb, sizeof(item->verb), verbp);
	fdr_copy_field(item->target, sizeof(item->target), targetp);
	fdr_copy_field(item->fpath, sizeof(item->fpath), fpath);
	item->line = line;

	if (strcmp(verbp, "instance") == 0) {
		item->type = FDR_ITEM_INSTANCE;
		fdr_copy_field(insp->iname, sizeof(insp->iname), targetp);
		if (fdr_join_path(insp->dname, sizeof(insp->dname), fdr.inst_dir,
		    targetp) >= (int)sizeof(insp->dname)) {
			fprintf(stderr, "%s:%d: instance path too long\n",
			    fpath, line);
			free(item);
			return -1;
		}
		insp->bufsize = 0;
		if (optarg != NULL) {
			insp->bufsize = fdr_parse_size(optarg);
			fprintf(stderr, "bufsize: %lu\n", insp->bufsize);
		}
	} else if (strcmp(verbp, "modprobe") == 0) {
		item->type = FDR_ITEM_MODPROBE;
	} else if (strcmp(verbp, "enable") == 0) {
		item->type = FDR_ITEM_ENABLE;
		if (optarg != NULL)
			fdr_copy_field(item->optarg, sizeof(item->optarg), optarg);
	} else if (strcmp(verbp, "disable") == 0) {
		item->type = FDR_ITEM_DISABLE;
	} else if (strcmp(verbp, "saveto") == 0) {
		item->type = FDR_ITEM_SAVETO;
		if (optarg != NULL)
			insp->maxsize = (long)fdr_parse_size(optarg);
		else
			insp->maxsize = FDR_MAXSIZE_DEFAULT;
		if (fdr.verbose > 1)
			fprintf(stderr, "maxsize: %ld\n", insp->maxsize);
	} else if (strcmp(verbp, "minfree") == 0) {
		item->type = FDR_ITEM_MINFREE;
		insp->minfree = atoi(targetp);
		if (insp->minfree > 100 || insp->minfree <= 0)
			insp->minfree = FDR_MINFREE_DEFAULT;
		if (fdr.verbose)
			fprintf(stderr, "minfree: %d\n", insp->minfree);
	} else {
		fprintf(stderr, "BAD verb at %d in %s\n", line, fpath);
		free(item);
		return -1;
	}

	insp->items = fdr_item_append(insp->items, item);
	return 0;
}

static int
fdr_read_config_file(const char *fpath, const struct stat *sb, int typeflag)
{
	FILE *fp;
	char linebuf[FDR_BUFSIZE];
	struct fdr_instance *insp;
	int line, rc;

	(void)sb;

	if (typeflag != FTW_F)
		return 0;

	{
		const char *suffix = strrchr(fpath, '.');

		if (suffix == NULL || strcmp(suffix, ".conf") != 0)
			return 0;
	}

	fprintf(stderr, "reading %s\n", fpath);

	fp = fopen(fpath, "r");
	if (fp == NULL) {
		perror(fpath);
		return 1;
	}

	insp = calloc(1, sizeof(*insp));
	if (insp == NULL) {
		perror("calloc");
		fclose(fp);
		return 1;
	}
	fdr_instance_init(insp);
	fdr_instance_append(insp);

	rc = 0;
	for (line = 1; fgets(linebuf, sizeof(linebuf), fp) != NULL; line++) {
		if (linebuf[0] == '#' || linebuf[0] == '\n')
			continue;

		if (fdr_parse_line(insp, fpath, line, linebuf) != 0) {
			rc = 1;
			break;
		}
	}

	if (rc == 0 && insp->iname[0] == '\0') {
		fprintf(stderr, "%s: missing instance directive\n", fpath);
		rc = 1;
	}

	fclose(fp);
	return rc;
}

int
fdr_config_load(const char *dir)
{
	int rc = ftw(dir, fdr_read_config_file, 1);

	if (rc != 0)
		fprintf(stderr, "configuration errors in %s\n", dir);
	else if (fdr.instance_count == 0)
		fprintf(stderr, "no instances found in %s\n", dir);

	return rc != 0 || fdr.instance_count == 0 ? -1 : 0;
}
