/*
 * config.c - strict configuration file parsing
 */

#include "fdr.h"

#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
fdr_item_append(struct fdr_instance *insp, struct fdr_item *item)
{
	struct fdr_item **tail = &insp->items;

	while (*tail != NULL)
		tail = &(*tail)->next;
	*tail = item;
}

static void
fdr_instance_free_one(struct fdr_instance *insp)
{
	struct fdr_item *item = insp->items;

	while (item != NULL) {
		struct fdr_item *next = item->next;

		free(item);
		item = next;
	}
	free(insp);
}

void
fdr_config_free(void)
{
	struct fdr_instance *insp = fdr.instances;

	while (insp != NULL) {
		struct fdr_instance *next = insp->next;

		fdr_instance_free_one(insp);
		insp = next;
	}
	fdr.instances = NULL;
	fdr.instance_count = 0;
}

static char *
fdr_trim(char *text)
{
	char *end;

	while (isspace((unsigned char)*text))
		text++;
	end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return text;
}

static char *
fdr_next_token(char **cursor)
{
	char *start = *cursor;
	char *end;

	while (isspace((unsigned char)*start))
		start++;
	if (*start == '\0') {
		*cursor = start;
		return NULL;
	}
	end = start;
	while (*end != '\0' && !isspace((unsigned char)*end))
		end++;
	if (*end != '\0')
		*end++ = '\0';
	*cursor = end;
	return start;
}

static int
fdr_valid_name(const char *name, int module_name)
{
	const unsigned char *p = (const unsigned char *)name;

	if (*p == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return 0;
	for (; *p != '\0'; p++) {
		if (isalnum(*p) || *p == '_' || *p == '-' || *p == '.')
			continue;
		if (module_name && *p == ':')
			continue;
		return 0;
	}
	return 1;
}

static int
fdr_valid_probe(const char *target)
{
	char copy[FDR_NAME_MAX * 2];
	char *slash;

	if (fdr_copy_field(copy, sizeof(copy), target) != 0)
		return 0;
	slash = strchr(copy, '/');
	if (slash == NULL || strchr(slash + 1, '/') != NULL)
		return 0;
	*slash++ = '\0';
	return fdr_valid_name(copy, 0) && fdr_valid_name(slash, 0);
}

static int
fdr_parse_error(const char *fpath, int line, const char *message)
{
	fdr_log("error", "%s:%d: %s", fpath, line, message);
	return -1;
}

static int
fdr_parse_line(struct fdr_instance *insp, const char *fpath, int line,
    char *linebuf)
{
	char *cursor, *verb, *target, *option;
	struct fdr_item *item;
	uint64_t size;

	fdr_chomp_line(linebuf);
	cursor = fdr_trim(linebuf);
	if (*cursor == '\0' || *cursor == '#')
		return 0;

	verb = fdr_next_token(&cursor);
	target = fdr_next_token(&cursor);
	option = fdr_trim(cursor);
	if (target == NULL)
		return fdr_parse_error(fpath, line, "directive requires a value");
	if (insp->items == NULL && strcmp(verb, "instance") != 0)
		return fdr_parse_error(fpath, line,
		    "the first directive must be 'instance'");

	item = calloc(1, sizeof(*item));
	if (item == NULL)
		return fdr_parse_error(fpath, line, "out of memory");
	if (fdr_copy_field(item->verb, sizeof(item->verb), verb) != 0 ||
	    fdr_copy_field(item->target, sizeof(item->target), target) != 0 ||
	    fdr_copy_field(item->fpath, sizeof(item->fpath), fpath) != 0) {
		free(item);
		return fdr_parse_error(fpath, line, "directive is too long");
	}
	item->line = line;

	if (strcmp(verb, "instance") == 0) {
		if (insp->items != NULL) {
			free(item);
			return fdr_parse_error(fpath, line,
			    "only one instance directive is allowed");
		}
		if (!fdr_valid_name(target, 0)) {
			free(item);
			return fdr_parse_error(fpath, line, "invalid instance name");
		}
		item->type = FDR_ITEM_INSTANCE;
		if (fdr_copy_field(insp->iname, sizeof(insp->iname), target) != 0 ||
		    fdr_join_path(insp->dname, sizeof(insp->dname), fdr.inst_dir,
		    target) != 0) {
			free(item);
			return fdr_parse_error(fpath, line, "instance path is too long");
		}
		if (*option != '\0') {
			if (fdr_parse_size(option, &size) != 0 || size == 0) {
				free(item);
				return fdr_parse_error(fpath, line,
				    "invalid instance buffer size");
			}
			if (size > UINT64_MAX - UINT64_C(1023)) {
				free(item);
				return fdr_parse_error(fpath, line,
				    "instance buffer size is too large");
			}
			insp->bufsize_kb = (size + UINT64_C(1023)) / 1024;
		}
	} else if (strcmp(verb, "modprobe") == 0) {
		if (*option != '\0' || !fdr_valid_name(target, 1)) {
			free(item);
			return fdr_parse_error(fpath, line, "invalid module name");
		}
		item->type = FDR_ITEM_MODPROBE;
	} else if (strcmp(verb, "enable") == 0) {
		if (!fdr_valid_probe(target)) {
			free(item);
			return fdr_parse_error(fpath, line,
			    "probe must be subsystem/event");
		}
		item->type = FDR_ITEM_ENABLE;
		if (*option != '\0' &&
		    fdr_copy_field(item->optarg, sizeof(item->optarg), option) != 0) {
			free(item);
			return fdr_parse_error(fpath, line, "filter is too long");
		}
	} else if (strcmp(verb, "disable") == 0) {
		if (*option != '\0' || !fdr_valid_probe(target)) {
			free(item);
			return fdr_parse_error(fpath, line,
			    "disable requires one subsystem/event value");
		}
		item->type = FDR_ITEM_DISABLE;
	} else if (strcmp(verb, "saveto") == 0) {
		if (target[0] != '/') {
			free(item);
			return fdr_parse_error(fpath, line,
			    "saveto path must be absolute");
		}
		if (insp->has_saveto) {
			free(item);
			return fdr_parse_error(fpath, line,
			    "only one saveto directive is allowed");
		}
		item->type = FDR_ITEM_SAVETO;
		insp->has_saveto = 1;
		if (*option != '\0') {
			if (fdr_parse_size(option, &size) != 0 || size == 0) {
				free(item);
				return fdr_parse_error(fpath, line,
				    "invalid saveto maximum size");
			}
			insp->maxsize = size;
		}
	} else if (strcmp(verb, "minfree") == 0) {
		char *end;
		long value;

		if (*option != '\0') {
			free(item);
			return fdr_parse_error(fpath, line,
			    "minfree accepts exactly one value");
		}
		errno = 0;
		value = strtol(target, &end, 10);
		if (errno != 0 || *end != '\0' || value < 1 || value > 100) {
			free(item);
			return fdr_parse_error(fpath, line,
			    "minfree must be between 1 and 100");
		}
		item->type = FDR_ITEM_MINFREE;
		insp->minfree = (int)value;
	} else {
		free(item);
		return fdr_parse_error(fpath, line, "unknown directive");
	}

	fdr_item_append(insp, item);
	return 0;
}

int
fdr_config_parse_line_test(struct fdr_instance *insp, const char *line)
{
	char copy[FDR_CONFIG_LINE_MAX];

	if (fdr_copy_field(copy, sizeof(copy), line) != 0)
		return -1;
	return fdr_parse_line(insp, "<test>", 1, copy);
}

static int
fdr_duplicate_instance(const char *name)
{
	struct fdr_instance *cur;

	for (cur = fdr.instances; cur != NULL; cur = cur->next) {
		if (strcmp(cur->iname, name) == 0)
			return 1;
	}
	return 0;
}

static int
fdr_read_config_file(const char *fpath)
{
	FILE *fp;
	char linebuf[FDR_CONFIG_LINE_MAX];
	struct fdr_instance *insp;
	struct stat st;
	int line = 0;
	int rc = -1;

	fp = fopen(fpath, "r");
	if (fp == NULL) {
		fdr_warn("cannot open %s: %s", fpath, strerror(errno));
		return -1;
	}
	if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode)) {
		fdr_warn("%s is not a regular file", fpath);
		fclose(fp);
		return -1;
	}

	insp = calloc(1, sizeof(*insp));
	if (insp == NULL) {
		fclose(fp);
		return -1;
	}
	fdr_instance_init(insp);

	while (fgets(linebuf, sizeof(linebuf), fp) != NULL) {
		line++;
		if (strchr(linebuf, '\n') == NULL && !feof(fp)) {
			int ch;

			while ((ch = fgetc(fp)) != '\n' && ch != EOF)
				;
			(void)fdr_parse_error(fpath, line, "line is too long");
			goto out;
		}
		if (fdr_parse_line(insp, fpath, line, linebuf) != 0)
			goto out;
	}
	if (ferror(fp)) {
		fdr_warn("cannot read %s: %s", fpath, strerror(errno));
		goto out;
	}
	if (insp->items == NULL || insp->iname[0] == '\0') {
		(void)fdr_parse_error(fpath, line, "missing instance directive");
		goto out;
	}
	if (fdr_duplicate_instance(insp->iname)) {
		(void)fdr_parse_error(fpath, line, "duplicate instance name");
		goto out;
	}
	if (fdr.instance_count >= FDR_MAX_CHILDREN) {
		(void)fdr_parse_error(fpath, line, "too many instances");
		goto out;
	}

	fdr_instance_append(insp);
	fdr_log("info", "loaded instance %s from %s", insp->iname, fpath);
	rc = 0;
out:
	fclose(fp);
	if (rc != 0)
		fdr_instance_free_one(insp);
	return rc;
}

int
fdr_config_load(const char *dir)
{
	char pattern[FDR_PATH_MAX];
	glob_t matches;
	size_t i;
	int glob_rc;
	int rc = 0;

	if (fdr_join_path(pattern, sizeof(pattern), dir, "*.conf") != 0) {
		fdr_warn("configuration directory path is too long");
		return -1;
	}
	memset(&matches, 0, sizeof(matches));
	glob_rc = glob(pattern, 0, NULL, &matches);
	if (glob_rc == GLOB_NOMATCH) {
		fdr_warn("no configuration files found in %s", dir);
		globfree(&matches);
		return -1;
	}
	if (glob_rc != 0) {
		fdr_warn("cannot scan configuration directory %s", dir);
		globfree(&matches);
		return -1;
	}

	for (i = 0; i < matches.gl_pathc; i++) {
		if (fdr_read_config_file(matches.gl_pathv[i]) != 0) {
			rc = -1;
			break;
		}
	}
	globfree(&matches);
	return rc;
}
