#include "fdr.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BENCH_CPUS_DEFAULT 256U
#define BENCH_ROUNDS_DEFAULT 50U

static uint64_t
elapsed_ns(const struct timespec *start, const struct timespec *finish)
{
	uint64_t seconds = (uint64_t)(finish->tv_sec - start->tv_sec);
	long nanoseconds = finish->tv_nsec - start->tv_nsec;

	if (nanoseconds < 0) {
		seconds--;
		nanoseconds += 1000000000L;
	}
	return seconds * UINT64_C(1000000000) + (uint64_t)nanoseconds;
}

static unsigned int
read_positive_env(const char *name, unsigned int default_value)
{
	const char *value = getenv(name);
	char *end;
	unsigned long parsed;

	if (value == NULL || *value == '\0')
		return default_value;
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
	    parsed > 4096)
		return 0;
	return (unsigned int)parsed;
}

static void
create_stats_file(const char *path)
{
	static const char contents[] =
	    "entries: 0\n"
	    "overrun: 0\n"
	    "commit overrun: 0\n"
	    "bytes: 0\n"
	    "dropped events: 0\n"
	    "read events: 0\n";
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

	assert(fd >= 0);
	assert(fdr_write_all(fd, contents, sizeof(contents) - 1) == 0);
	assert(close(fd) == 0);
}

static void
create_cpu_tree(const char *per_cpu, unsigned int cpus)
{
	unsigned int cpu;

	assert(mkdir(per_cpu, 0700) == 0);
	for (cpu = 0; cpu < cpus; cpu++) {
		char name[32];
		char cpu_dir[FDR_PATH_MAX];
		char stats[FDR_PATH_MAX];

		assert(snprintf(name, sizeof(name), "cpu%u", cpu) <
		    (int)sizeof(name));
		assert(fdr_join_path(cpu_dir, sizeof(cpu_dir), per_cpu, name) == 0);
		assert(fdr_join_path(stats, sizeof(stats), cpu_dir, "stats") == 0);
		assert(mkdir(cpu_dir, 0700) == 0);
		create_stats_file(stats);
	}
}

static void
remove_cpu_tree(const char *per_cpu, unsigned int cpus)
{
	unsigned int cpu;

	for (cpu = 0; cpu < cpus; cpu++) {
		char name[32];
		char cpu_dir[FDR_PATH_MAX];
		char stats[FDR_PATH_MAX];

		assert(snprintf(name, sizeof(name), "cpu%u", cpu) <
		    (int)sizeof(name));
		assert(fdr_join_path(cpu_dir, sizeof(cpu_dir), per_cpu, name) == 0);
		assert(fdr_join_path(stats, sizeof(stats), cpu_dir, "stats") == 0);
		assert(unlink(stats) == 0);
		assert(rmdir(cpu_dir) == 0);
	}
	assert(rmdir(per_cpu) == 0);
}

int
main(void)
{
	char tempdir[] = "/tmp/fdr-loss-benchmark-XXXXXX";
	char per_cpu[FDR_PATH_MAX];
	struct fdr_instance instance;
	struct timespec start;
	struct timespec finish;
	unsigned int cpus = read_positive_env("FDR_BENCH_CPUS",
	    BENCH_CPUS_DEFAULT);
	unsigned int rounds = read_positive_env("FDR_BENCH_LOSS_ROUNDS",
	    BENCH_ROUNDS_DEFAULT);
	uint64_t cpu_ns;
	unsigned int round;

	if (cpus == 0 || rounds == 0) {
		fprintf(stderr, "invalid benchmark CPU or round count\n");
		return 2;
	}
	assert(mkdtemp(tempdir) != NULL);
	assert(fdr_join_path(per_cpu, sizeof(per_cpu), tempdir, "per_cpu") == 0);
	create_cpu_tree(per_cpu, cpus);

	fdr_instance_init(&instance);
	assert(fdr_copy_field(instance.iname, sizeof(instance.iname),
	    "fdr-loss-benchmark") == 0);
	assert(fdr_copy_field(instance.dname, sizeof(instance.dname), tempdir) == 0);
	fdr_metrics_init();
	assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start) == 0);
	for (round = 0; round < rounds; round++)
		assert(fdr_trace_sample_loss(&instance) == 0);
	assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &finish) == 0);
	cpu_ns = elapsed_ns(&start, &finish);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_overruns) == 0);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_dropped_events) == 0);
	assert(fdr_metrics_load_u64(&fdr.metrics->trace_commit_overruns) == 0);
	printf("cpus=%u rounds=%u cpu_ns=%" PRIu64
	    " ns_per_cpu_sample=%.2f\n", cpus, rounds, cpu_ns,
	    (double)cpu_ns / ((double)cpus * (double)rounds));

	fdr_metrics_destroy();
	remove_cpu_tree(per_cpu, cpus);
	assert(rmdir(tempdir) == 0);
	return 0;
}
