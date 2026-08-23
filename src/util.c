/*
 * util.c - string and path helpers
 */

#include "fdr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void
fdr_copy_field(char *dst, size_t dstsz, const char *src)
{
	snprintf(dst, dstsz, "%s", src);
}

int
fdr_join_path(char *dst, size_t dstsz, const char *a, const char *b)
{
	return snprintf(dst, dstsz, "%s/%s", a, b);
}

void
fdr_chomp_line(char *buf)
{
	size_t len = strlen(buf);

	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';
}

unsigned long
fdr_parse_size(const char *arg)
{
	unsigned long x;
	char *ep;

	x = strtoul(arg, &ep, 0);
	if (*ep == 'k' || *ep == 'K')
		x *= 1024;
	else if (*ep == 'm' || *ep == 'M')
		x *= 1024UL * 1024UL;
	else if (*ep == 'g' || *ep == 'G')
		x *= 1024UL * 1024UL * 1024UL;

	return x;
}

const char *
fdr_default_inst_dir(void)
{
	if (access(FDR_DEBUG_INST_DIR, F_OK) == 0)
		return FDR_DEBUG_INST_DIR;
	if (access(FDR_TRACE_INST_DIR, F_OK) == 0)
		return FDR_TRACE_INST_DIR;
	return FDR_TRACE_INST_DIR;
}

void
fdr_instance_init(struct fdr_instance *insp)
{
	memset(insp, 0, sizeof(*insp));
	insp->minfree = FDR_MINFREE_DEFAULT;
	insp->maxsize = FDR_MAXSIZE_DEFAULT;
	insp->trace_fd = -1;
}

void
fdr_instance_append(struct fdr_instance *insp)
{
	insp->next = fdr.instances;
	fdr.instances = insp;
	fdr.instance_count++;
}
