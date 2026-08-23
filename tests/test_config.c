#include "fdr.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
free_items(struct fdr_instance *insp)
{
	struct fdr_item *item = insp->items;

	while (item != NULL) {
		struct fdr_item *next = item->next;

		free(item);
		item = next;
	}
	insp->items = NULL;
}

int
main(void)
{
	struct fdr_instance instance;
	struct fdr_instance invalid;
	uint64_t size = 0;

	assert(fdr_parse_size("1", &size) == 0 && size == 1);
	assert(fdr_parse_size("64k", &size) == 0 && size == 65536);
	assert(fdr_parse_size("2MiB", &size) == 0 && size == 2097152);
	assert(fdr_parse_size("1g", &size) == 0 &&
	    size == UINT64_C(1073741824));
	assert(fdr_parse_size("", &size) != 0);
	assert(fdr_parse_size("-1", &size) != 0);
	assert(fdr_parse_size("1mbad", &size) != 0);
	assert(fdr_parse_size("18446744073709551615g", &size) != 0);

	assert(fdr_copy_field(fdr.inst_dir, sizeof(fdr.inst_dir),
	    "/tmp/fdr-test/instances") == 0);
	fdr_instance_init(&instance);
	assert(fdr_config_parse_line_test(&instance, "  # comment") == 0);
	assert(instance.items == NULL);
	assert(fdr_config_parse_line_test(&instance, "instance node 64m") == 0);
	assert(strcmp(instance.iname, "node") == 0);
	assert(instance.bufsize_kb == 65536);
	assert(fdr_config_parse_line_test(&instance,
	    "enable sched/sched_switch prev_pid > 0") == 0);
	assert(fdr_config_parse_line_test(&instance, "minfree 9") == 0);
	assert(instance.minfree == 9);
	assert(fdr_config_parse_line_test(&instance,
	    "saveto /tmp/fdr.log 8MiB") == 0);
	assert(instance.maxsize == UINT64_C(8388608));
	assert(fdr_config_parse_line_test(&instance,
	    "saveto /tmp/second.log") != 0);
	free_items(&instance);

	fdr_instance_init(&invalid);
	assert(fdr_config_parse_line_test(&invalid,
	    "enable sched/sched_switch") != 0);
	assert(fdr_config_parse_line_test(&invalid, "instance") != 0);
	assert(fdr_config_parse_line_test(&invalid,
	    "instance ../../escape") != 0);
	assert(fdr_config_parse_line_test(&invalid, "instance node bad-size") != 0);
	assert(fdr_config_parse_line_test(&invalid, "instance node") == 0);
	assert(fdr_config_parse_line_test(&invalid, "minfree 0") != 0);
	assert(fdr_config_parse_line_test(&invalid, "enable sched") != 0);
	assert(fdr_config_parse_line_test(&invalid, "disable sched/all extra") != 0);
	assert(fdr_config_parse_line_test(&invalid, "modprobe x;touch") != 0);
	free_items(&invalid);

	puts("configuration tests passed");
	return 0;
}
