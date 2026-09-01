/*
 * test_trace.c - Unit test suite for mock tracefs probe configuration and kernel loss parsing
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Test Scope:
 * 1. Tracepoint Probe Enablement (`fdr_trace_set_probe`):
 *    - Verifies targeted event enablement without accidentally enabling the whole subsystem.
 *    - Verifies filter sequencing invariant: filter must succeed before probe is enabled;
 *      failure increments `probe_failures` metric and aborts enablement.
 * 2. Per-CPU Loss Sampling (`fdr_trace_sample_loss`):
 *    - Aggregates multi-core `cpu0/stats` and `cpu1/stats` (overrun, dropped events, commit overruns).
 *    - Verifies Prometheus counter accumulation and automatic readiness degradation (`healthy = 0`).
 *    - Tests non-negative counter delta handling during simulated kernel counter resets.
 */

#include "fdr.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * make_dir - Creates a directory with mode 0700 and asserts success.
 *
 * @path: Directory path to create.
 */
static void
make_dir(const char *path)
{
	assert(mkdir(path, 0700) == 0);
}

/**
 * make_control - Creates a mock tracefs control file with initial string content.
 *
 * @path: Filesystem path to create.
 * @value: String value to write.
 */
static void
make_control(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	assert(fd >= 0);
	assert(fdr_write_all(fd, value, strlen(value)) == 0);
	assert(close(fd) == 0);
}

/**
 * read_control - Reads the first character of a control file.
 *
 * @path: Filesystem path to read from.
 * Return: First character read from file.
 */
static char
read_control(const char *path)
{
	char value = '\0';
	int fd = open(path, O_RDONLY);

	assert(fd >= 0);
	assert(read(fd, &value, 1) == 1);
	assert(close(fd) == 0);
	return value;
}

int
main(void)
{
	char tempdir[] = "/tmp/fdr-trace-XXXXXX";
	char events[FDR_PATH_MAX];
	char subsystem[FDR_PATH_MAX];
	char event[FDR_PATH_MAX];
	char subsystem_enable[FDR_PATH_MAX];
	char event_enable[FDR_PATH_MAX];
	char event_filter[FDR_PATH_MAX];
	char per_cpu[FDR_PATH_MAX];
	char cpu0[FDR_PATH_MAX];
	char cpu1[FDR_PATH_MAX];
	char cpu2[FDR_PATH_MAX];
	char stats0[FDR_PATH_MAX];
	char stats1[FDR_PATH_MAX];
	char stats2[FDR_PATH_MAX];
	struct fdr_instance instance;
	struct fdr_item item;
	int attempt;

	assert(mkdtemp(tempdir) != NULL);
	assert(fdr_join_path(events, sizeof(events), tempdir, "events") == 0);
	assert(fdr_join_path(subsystem, sizeof(subsystem), events, "sched") == 0);
	assert(fdr_join_path(event, sizeof(event), subsystem, "allocate") == 0);
	assert(fdr_join_path(subsystem_enable, sizeof(subsystem_enable), subsystem,
	    "enable") == 0);
	assert(fdr_join_path(event_enable, sizeof(event_enable), event,
	    "enable") == 0);
	assert(fdr_join_path(event_filter, sizeof(event_filter), event,
	    "filter") == 0);

	/* Construct mock tracefs hierarchy: events/sched/allocate/enable */
	make_dir(events);
	make_dir(subsystem);
	make_dir(event);
	make_control(subsystem_enable, "0");
	make_control(event_enable, "0");

	fdr_instance_init(&instance);
	assert(fdr_copy_field(instance.iname, sizeof(instance.iname), "test") == 0);
	assert(fdr_copy_field(instance.dname, sizeof(instance.dname), tempdir) == 0);
	memset(&item, 0, sizeof(item));
	item.type = FDR_ITEM_ENABLE;
	assert(fdr_copy_field(item.target, sizeof(item.target),
	    "sched/allocate") == 0);

	/* --- Test Suite 1: Targeted Probe Enablement --- */
	assert(fdr_trace_set_probe(&instance, &item) == 0);
	assert(read_control(event_enable) == '1');
	assert(read_control(subsystem_enable) == '0');

	/* --- Test Suite 2: Filter Invariant & Failure Handling --- */
	make_control(event_enable, "0");
	assert(fdr_copy_field(item.optarg, sizeof(item.optarg),
	    "pid > 0") == 0);
	fdr_metrics_init();
	/* Fails because event_filter node does not exist in mock tracefs */
	assert(fdr_trace_set_probe(&instance, &item) != 0);
	assert(read_control(event_enable) == '0');
	assert(fdr_metrics_load_u64(&fdr.metrics->probe_failures) == 1);

	/* --- Test Suite 3: Per-CPU Loss Metric Parsing & Multi-Core Summing --- */
	assert(fdr_join_path(per_cpu, sizeof(per_cpu), tempdir, "per_cpu") == 0);
	assert(fdr_join_path(cpu0, sizeof(cpu0), per_cpu, "cpu0") == 0);
	assert(fdr_join_path(cpu1, sizeof(cpu1), per_cpu, "cpu1") == 0);
	assert(fdr_join_path(cpu2, sizeof(cpu2), per_cpu, "cpu2") == 0);
	assert(fdr_join_path(stats0, sizeof(stats0), cpu0, "stats") == 0);
	assert(fdr_join_path(stats1, sizeof(stats1), cpu1, "stats") == 0);
	assert(fdr_join_path(stats2, sizeof(stats2), cpu2, "stats") == 0);
	make_dir(per_cpu);
	make_dir(cpu0);
	make_dir(cpu1);
	make_dir(cpu2);
	make_control(stats0,
	    "entries: 0\noverrun: 2\ncommit overrun: 1\n"
	    "bytes: 0\ndropped events: 3\nread events: 0\n");
	make_control(stats1,
	    "entries: 0\noverrun: 5\ncommit overrun: 0\n"
	    "bytes: 0\ndropped events: 7\nread events: 0\n");

	fdr_metrics_store_int(&fdr.metrics->healthy, 1);
	assert(fdr_trace_sample_loss(&instance) == 1);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_overruns) == 7);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_dropped_events) == 10);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_commit_overruns) == 1);
	assert(fdr_metrics_load_int(&fdr.metrics->healthy) == 0);

	/* Re-sampling with unchanged counters should return 0 (no new loss) */
	assert(fdr_trace_sample_loss(&instance) == 0);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_overruns) == 7);

	/* --- Test Suite 4: Kernel Counter Reset Delta Handling --- */
	make_control(stats0,
	    "overrun: 1\ncommit overrun: 0\ndropped events: 2\n");
	make_control(stats1,
	    "overrun: 0\ncommit overrun: 0\ndropped events: 0\n");
	assert(fdr_trace_sample_loss(&instance) == 1);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_overruns) == 8);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_dropped_events) == 12);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_commit_overruns) == 1);

	make_control(stats0,
	    "overrun: 1\ncommit overrun: 3\ndropped events: 2\n");
	assert(fdr_trace_sample_loss(&instance) == 1);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_commit_overruns) == 4);

	/* --- Test Suite 5: Cached Topology Refresh & CPU Hotplug --- */
	make_control(stats2,
	    "overrun: 4\ncommit overrun: 0\ndropped events: 5\n");
	for (attempt = 0; attempt < 16; attempt++) {
		if (fdr_trace_sample_loss(&instance) == 1)
			break;
	}
	assert(attempt < 16);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_overruns) == 12);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_dropped_events) == 17);

	/* A disappearing cached CPU path triggers an immediate safe rediscovery. */
	assert(unlink(stats1) == 0);
	assert(rmdir(cpu1) == 0);
	assert(fdr_trace_sample_loss(&instance) == 0);
	fdr_trace_reset_loss_cache(&instance);
	fdr_metrics_destroy();

	/* Cleanup mock directory hierarchy */
	assert(unlink(stats2) == 0);
	assert(unlink(stats0) == 0);
	assert(rmdir(cpu0) == 0);
	assert(rmdir(cpu2) == 0);
	assert(rmdir(per_cpu) == 0);
	assert(unlink(event_enable) == 0);
	assert(unlink(subsystem_enable) == 0);
	assert(access(event_filter, F_OK) != 0);
	assert(rmdir(event) == 0);
	assert(rmdir(subsystem) == 0);
	assert(rmdir(events) == 0);
	assert(rmdir(tempdir) == 0);
	puts("trace tests passed");
	return 0;
}
