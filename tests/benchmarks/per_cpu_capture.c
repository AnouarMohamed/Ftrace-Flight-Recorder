#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PATH_LIMIT 4096
#define TEXT_READ_SIZE 8192U
#define RAW_SPLICE_SIZE (1024U * 1024U)
#define MAX_CPUS 4096U

enum capture_mode {
	CAPTURE_TEXT,
	CAPTURE_RAW,
};

struct cpu_capture {
	pthread_t thread;
	enum capture_mode mode;
	int cpu;
	int input_fd;
	int output_fd;
	int raw_pipe[2];
	uint64_t bytes;
	uint64_t operations;
	int error_number;
};

static volatile sig_atomic_t stop_requested;

static void
stop_handler(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static int
cpu_number(const char *name)
{
	const unsigned char *cursor;
	unsigned long value = 0;

	if (strncmp(name, "cpu", 3) != 0 || name[3] == '\0')
		return -1;
	for (cursor = (const unsigned char *)name + 3; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			return -1;
		value = value * 10 + (unsigned long)(*cursor - '0');
		if (value >= MAX_CPUS)
			return -1;
	}
	return (int)value;
}

static int
compare_capture(const void *left, const void *right)
{
	const struct cpu_capture *a = left;
	const struct cpu_capture *b = right;

	return (a->cpu > b->cpu) - (a->cpu < b->cpu);
}

static int
write_all(int fd, const char *buffer, size_t length)
{
	size_t offset = 0;

	while (offset < length) {
		ssize_t n = write(fd, buffer + offset, length - offset);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		offset += (size_t)n;
	}
	return 0;
}

static void *
capture_cpu(void *opaque)
{
	struct cpu_capture *capture = opaque;
	char buffer[TEXT_READ_SIZE];
	struct pollfd pfd = {
		.fd = capture->input_fd,
		.events = POLLIN,
	};

	while (!stop_requested) {
		ssize_t n;

		if (capture->mode == CAPTURE_RAW) {
			n = splice(capture->input_fd, NULL, capture->raw_pipe[1], NULL,
			    RAW_SPLICE_SIZE, SPLICE_F_MOVE | SPLICE_F_MORE |
			    SPLICE_F_NONBLOCK);
		} else {
			n = read(capture->input_fd, buffer, sizeof(buffer));
			if (n > 0 && write_all(capture->output_fd, buffer,
			    (size_t)n) != 0) {
				capture->error_number = errno != 0 ? errno : EIO;
				break;
			}
		}
		if (capture->mode == CAPTURE_RAW && n > 0) {
			ssize_t remaining = n;

			while (remaining > 0) {
				ssize_t moved = splice(capture->raw_pipe[0], NULL,
				    capture->output_fd, NULL, (size_t)remaining,
				    SPLICE_F_MOVE | SPLICE_F_MORE);

				if (moved < 0 && errno == EINTR)
					continue;
				if (moved <= 0) {
					capture->error_number = errno != 0 ? errno : EIO;
					break;
				}
				remaining -= moved;
			}
			if (capture->error_number != 0)
				break;
		}
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				int ready = poll(&pfd, 1, 100);

				if (ready >= 0 || errno == EINTR)
					continue;
				capture->error_number = errno;
				break;
			}
			capture->error_number = errno;
			break;
		}
		if (n == 0) {
			(void)poll(&pfd, 1, 100);
			continue;
		}
		capture->bytes += (uint64_t)n;
		capture->operations++;
	}
	if (close(capture->input_fd) != 0 && capture->error_number == 0)
		capture->error_number = errno;
	if (close(capture->output_fd) != 0 && capture->error_number == 0)
		capture->error_number = errno;
	if (capture->mode == CAPTURE_RAW) {
		(void)close(capture->raw_pipe[0]);
		(void)close(capture->raw_pipe[1]);
	}
	return NULL;
}

static int
open_capture(struct cpu_capture *capture, const char *instance,
    const char *output_dir, enum capture_mode mode)
{
	const char *input_name = mode == CAPTURE_RAW ? "trace_pipe_raw" :
	    "trace_pipe";
	const char *suffix = mode == CAPTURE_RAW ? "raw" : "text";
	char input[PATH_LIMIT];
	char output[PATH_LIMIT];
	int output_flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;

#ifdef O_NOFOLLOW
	output_flags |= O_NOFOLLOW;
#endif
	if (snprintf(input, sizeof(input), "%s/per_cpu/cpu%d/%s", instance,
	    capture->cpu, input_name) >= (int)sizeof(input) ||
	    snprintf(output, sizeof(output), "%s/cpu%d.%s", output_dir,
	    capture->cpu, suffix) >= (int)sizeof(output)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	capture->mode = mode;
	capture->raw_pipe[0] = -1;
	capture->raw_pipe[1] = -1;
	capture->input_fd = open(input, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (capture->input_fd < 0)
		return -1;
	capture->output_fd = open(output, output_flags, 0600);
	if (capture->output_fd < 0) {
		int saved_errno = errno;

		(void)close(capture->input_fd);
		errno = saved_errno;
		return -1;
	}
	if (mode == CAPTURE_RAW && pipe2(capture->raw_pipe,
	    O_NONBLOCK | O_CLOEXEC) != 0) {
		int saved_errno = errno;

		(void)close(capture->input_fd);
		(void)close(capture->output_fd);
		errno = saved_errno;
		return -1;
	}
	return 0;
}

static int
discover_cpus(const char *instance, struct cpu_capture **captures_out,
    size_t *count_out)
{
	char path[PATH_LIMIT];
	struct cpu_capture *captures = NULL;
	size_t count = 0;
	DIR *directory;
	struct dirent *entry;

	if (snprintf(path, sizeof(path), "%s/per_cpu", instance) >=
	    (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	directory = opendir(path);
	if (directory == NULL)
		return -1;
	while ((entry = readdir(directory)) != NULL) {
		struct cpu_capture *resized;
		int cpu = cpu_number(entry->d_name);

		if (cpu < 0)
			continue;
		resized = realloc(captures, (count + 1) * sizeof(*captures));
		if (resized == NULL) {
			free(captures);
			(void)closedir(directory);
			return -1;
		}
		captures = resized;
		memset(&captures[count], 0, sizeof(captures[count]));
		captures[count].cpu = cpu;
		captures[count].input_fd = -1;
		captures[count].output_fd = -1;
		captures[count].raw_pipe[0] = -1;
		captures[count].raw_pipe[1] = -1;
		count++;
	}
	if (closedir(directory) != 0) {
		free(captures);
		return -1;
	}
	if (count == 0) {
		free(captures);
		errno = ENODEV;
		return -1;
	}
	qsort(captures, count, sizeof(*captures), compare_capture);
	*captures_out = captures;
	*count_out = count;
	return 0;
}

int
main(int argc, char **argv)
{
	struct cpu_capture *captures;
	struct sigaction action;
	struct stat output_stat;
	struct timespec pause_time = { .tv_sec = 0, .tv_nsec = 100000000L };
	enum capture_mode mode;
	size_t count;
	size_t opened = 0;
	size_t started = 0;
	size_t i;
	int failed = 0;

	if (argc != 4 || (strcmp(argv[1], "text") != 0 &&
	    strcmp(argv[1], "raw") != 0)) {
		fprintf(stderr, "usage: %s text|raw trace-instance output-directory\n",
		    argv[0]);
		return 2;
	}
	mode = strcmp(argv[1], "raw") == 0 ? CAPTURE_RAW : CAPTURE_TEXT;
	if (stat(argv[3], &output_stat) != 0 || !S_ISDIR(output_stat.st_mode)) {
		fprintf(stderr, "output is not a directory: %s\n", argv[3]);
		return 2;
	}
	if (discover_cpus(argv[2], &captures, &count) != 0) {
		fprintf(stderr, "cannot discover per-CPU readers: %s\n",
		    strerror(errno));
		return 3;
	}
	for (opened = 0; opened < count; opened++) {
		if (open_capture(&captures[opened], argv[2], argv[3], mode) != 0) {
			fprintf(stderr, "cannot open %s reader for cpu%d: %s\n", argv[1],
			    captures[opened].cpu, strerror(errno));
			failed = 1;
			break;
		}
	}
	if (failed)
		goto close_opened;

	memset(&action, 0, sizeof(action));
	action.sa_handler = stop_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGTERM, &action, NULL) != 0 ||
	    sigaction(SIGINT, &action, NULL) != 0) {
		fprintf(stderr, "cannot install signal handler: %s\n", strerror(errno));
		failed = 1;
		goto close_opened;
	}
	for (started = 0; started < count; started++) {
		if (pthread_create(&captures[started].thread, NULL, capture_cpu,
		    &captures[started]) != 0) {
			fprintf(stderr, "cannot start cpu%d collector\n",
			    captures[started].cpu);
			failed = 1;
			stop_requested = 1;
			break;
		}
	}
	printf("ready mode=%s cpus=%zu\n", argv[1], count);
	fflush(stdout);
	while (!stop_requested)
		(void)nanosleep(&pause_time, NULL);
	for (i = 0; i < started; i++)
		(void)pthread_join(captures[i].thread, NULL);
	for (i = 0; i < count; i++) {
		printf("cpu=%d bytes=%" PRIu64 " operations=%" PRIu64 " error=%d\n",
		    captures[i].cpu, captures[i].bytes, captures[i].operations,
		    captures[i].error_number);
		if (captures[i].error_number != 0)
			failed = 1;
	}
	free(captures);
	return failed ? 1 : 0;

close_opened:
	for (i = 0; i < opened; i++) {
		(void)close(captures[i].input_fd);
		(void)close(captures[i].output_fd);
		if (captures[i].raw_pipe[0] >= 0)
			(void)close(captures[i].raw_pipe[0]);
		if (captures[i].raw_pipe[1] >= 0)
			(void)close(captures[i].raw_pipe[1]);
	}
	free(captures);
	return 3;
}
