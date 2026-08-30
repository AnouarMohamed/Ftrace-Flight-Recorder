/*
 * test_config.c - Unit test suite for configuration parsing and numeric size conversions
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Test Scope:
 * 1. Human-Readable Size Parsing (`fdr_parse_size`):
 *    - Valid raw bytes ("1"), binary units ("64k", "2MiB", "1g").
 *    - Invalid strings (empty, negative, alphanumeric garbage, 64-bit overflow).
 * 2. Configuration Line Parsing (`fdr_config_parse_line_test`):
 *    - Comment and blank line handling.
 *    - Valid directives: `instance`, `enable` with filters, `minfree`, `saveto` with limits.
 *    - Rejection of multiple `saveto` directives.
 *    - Rejection of invalid syntax: probes before instance, directory traversal names,
 *      out-of-range minfree values, malformed subsystem/event identifiers, shell metacharacters in modprobe.
 */

#include "fdr.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * free_items - Helper to clean up AST directive items for a test instance.
 *
 * @insp: Pointer to instance structure containing items list.
 */
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

	/* --- Test Suite 1: Size Parser Unit Tests (fdr_parse_size) --- */
	assert(fdr_parse_size("1", &size) == 0 && size == 1);
	assert(fdr_parse_size("64k", &size) == 0 && size == 65536);
	assert(fdr_parse_size("2MiB", &size) == 0 && size == 2097152);
	assert(fdr_parse_size("1g", &size) == 0 &&
	    size == UINT64_C(1073741824));

	/* Invalid size strings must be rejected cleanly */
	assert(fdr_parse_size("", &size) != 0);
	assert(fdr_parse_size("-1", &size) != 0);
	assert(fdr_parse_size("1mbad", &size) != 0);
	assert(fdr_parse_size("18446744073709551615g", &size) != 0);

	/* --- Test Suite 2: Valid Configuration Directives --- */
	assert(fdr_copy_field(fdr.inst_dir, sizeof(fdr.inst_dir),
	    "/tmp/fdr-test/instances") == 0);
	fdr_instance_init(&instance);

	/* Verify comments are ignored */
	assert(fdr_config_parse_line_test(&instance, "  # comment") == 0);
	assert(instance.items == NULL);

	/* First directive: instance definition */
	assert(fdr_config_parse_line_test(&instance, "instance node 64m") == 0);
	assert(strcmp(instance.iname, "node") == 0);
	assert(instance.bufsize_kb == 65536);

	/* Tracepoint with relational filter expression */
	assert(fdr_config_parse_line_test(&instance,
	    "enable sched/sched_switch prev_pid > 0") == 0);

	/* Filesystem minfree percentage threshold */
	assert(fdr_config_parse_line_test(&instance, "minfree 9") == 0);
	assert(instance.minfree == 9);

	/* Saveto destination path with bounded maximum file size */
	assert(fdr_config_parse_line_test(&instance,
	    "saveto /tmp/fdr.log 8MiB") == 0);
	assert(instance.maxsize == UINT64_C(8388608));

	/* Rejection of duplicate saveto directives */
	assert(fdr_config_parse_line_test(&instance,
	    "saveto /tmp/second.log") != 0);
	free_items(&instance);

	/* --- Test Suite 3: Invalid Syntax & Security Violations --- */
	fdr_instance_init(&invalid);

	/* Rejection: probe directive before 'instance' */
	assert(fdr_config_parse_line_test(&invalid,
	    "enable sched/sched_switch") != 0);

	/* Rejection: missing instance value */
	assert(fdr_config_parse_line_test(&invalid, "instance") != 0);

	/* Rejection: path traversal attempt in instance name */
	assert(fdr_config_parse_line_test(&invalid,
	    "instance ../../escape") != 0);

	/* Rejection: invalid buffer size string */
	assert(fdr_config_parse_line_test(&invalid, "instance node bad-size") != 0);

	/* Valid instance initialization to test subsequent invalid directives */
	assert(fdr_config_parse_line_test(&invalid, "instance node") == 0);

	/* Rejection: out-of-bounds minfree percentage (< 1 or > 100) */
	assert(fdr_config_parse_line_test(&invalid, "minfree 0") != 0);

	/* Rejection: malformed probe without '/' */
	assert(fdr_config_parse_line_test(&invalid, "enable sched") != 0);

	/* Rejection: unexpected extra arguments to 'disable' */
	assert(fdr_config_parse_line_test(&invalid, "disable sched/all extra") != 0);

	/* Rejection: shell metacharacters in modprobe */
	assert(fdr_config_parse_line_test(&invalid, "modprobe x;touch") != 0);
	free_items(&invalid);

	puts("configuration tests passed");
	return 0;
}

