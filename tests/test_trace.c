#include "fdr.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
make_dir(const char *path)
{
	assert(mkdir(path, 0700) == 0);
}

static void
make_control(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	assert(fd >= 0);
	assert(fdr_write_all(fd, value, strlen(value)) == 0);
	assert(close(fd) == 0);
}

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
	struct fdr_instance instance;
	struct fdr_item item;

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
	assert(fdr_trace_set_probe(&instance, &item) == 0);
	assert(read_control(event_enable) == '1');
	assert(read_control(subsystem_enable) == '0');

	make_control(event_enable, "0");
	assert(fdr_copy_field(item.optarg, sizeof(item.optarg),
	    "pid > 0") == 0);
	fdr_metrics_init();
	assert(fdr_trace_set_probe(&instance, &item) != 0);
	assert(read_control(event_enable) == '0');
	assert(fdr_metrics_load_u64(&fdr.metrics->probe_failures) == 1);

	assert(fdr_join_path(per_cpu, sizeof(per_cpu), tempdir, "per_cpu") == 0);
	assert(fdr_join_path(cpu0, sizeof(cpu0), per_cpu, "cpu0") == 0);
	assert(fdr_join_path(cpu1, sizeof(cpu1), per_cpu, "cpu1") == 0);
	assert(fdr_join_path(cpu2, sizeof(cpu2), per_cpu, "cpu2") == 0);
	assert(fdr_join_path(stats0, sizeof(stats0), cpu0, "stats") == 0);
	assert(fdr_join_path(stats1, sizeof(stats1), cpu1, "stats") == 0);
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
	assert(fdr_trace_sample_loss(&instance) == 0);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_overruns) == 7);

	/* A lower current value indicates that the kernel counter was reset. */
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
	fdr_metrics_destroy();

	assert(unlink(stats1) == 0);
	assert(unlink(stats0) == 0);
	assert(rmdir(cpu1) == 0);
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
