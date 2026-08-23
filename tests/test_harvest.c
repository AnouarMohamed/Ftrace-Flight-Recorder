#include "fdr.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
write_trace(const char *path, const char *payload)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	assert(fd >= 0);
	assert(fdr_write_all(fd, payload, strlen(payload)) == 0);
	assert(close(fd) == 0);
}

int
main(void)
{
	char tempdir[] = "/tmp/fdr-harvest-XXXXXX";
	char tracepipe[FDR_PATH_MAX];
	char logfile[FDR_PATH_MAX];
	char backup[FDR_PATH_MAX];
	struct fdr_instance instance;
	struct fdr_item item;
	struct stat st;
	const char payload[] = "trace event\n";

	assert(mkdtemp(tempdir) != NULL);
	assert(fdr_join_path(tracepipe, sizeof(tracepipe), tempdir,
	    "trace_pipe") == 0);
	assert(fdr_join_path(logfile, sizeof(logfile), tempdir, "output.log") == 0);
	assert(snprintf(backup, sizeof(backup), "%s.1", logfile) <
	    (int)sizeof(backup));
	write_trace(tracepipe, payload);

	fdr_instance_init(&instance);
	assert(fdr_copy_field(instance.iname, sizeof(instance.iname),
	    "fdr-unit-no-logrotate") == 0);
	assert(fdr_copy_field(instance.dname, sizeof(instance.dname), tempdir) == 0);
	instance.minfree = 100;
	memset(&item, 0, sizeof(item));
	item.type = FDR_ITEM_SAVETO;
	assert(fdr_copy_field(item.target, sizeof(item.target), logfile) == 0);

	fdr_metrics_init();
	assert(fdr_harvest_run(&instance, &item) == 0);
	assert(stat(logfile, &st) == 0 && st.st_size == 0);
	assert(fdr_metrics_load_u64(&fdr.metrics->bytes_dropped) ==
	    strlen(payload));

	write_trace(tracepipe, payload);
	instance.minfree = 0; /* Direct test-only setting: disable the guard. */
	assert(fdr_harvest_run(&instance, &item) == 0);
	assert(stat(logfile, &st) == 0 &&
	    st.st_size == (off_t)strlen(payload));
	assert(fdr_metrics_load_u64(&fdr.metrics->bytes_written) ==
	    strlen(payload));

	write_trace(tracepipe, payload);
	instance.maxsize = strlen(payload);
	assert(fdr_harvest_run(&instance, &item) == 0);
	assert(stat(logfile, &st) == 0 &&
	    st.st_size == (off_t)strlen(payload));
	assert(stat(backup, &st) == 0 &&
	    st.st_size == (off_t)strlen(payload));
	assert(fdr_metrics_load_u64(&fdr.metrics->rotations) == 1);
	fdr_metrics_destroy();

	assert(unlink(tracepipe) == 0);
	assert(unlink(logfile) == 0);
	assert(unlink(backup) == 0);
	assert(rmdir(tempdir) == 0);
	puts("harvest tests passed");
	return 0;
}
