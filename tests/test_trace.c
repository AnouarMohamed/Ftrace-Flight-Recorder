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
	fdr_metrics_destroy();

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
