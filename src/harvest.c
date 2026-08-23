/*
 * harvest.c - trace_pipe reader and log rotation
 */

#include "fdr.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

static void
fdr_sighup_worker(int signo, siginfo_t *info, void *ctx)
{
	(void)signo;
	(void)info;
	(void)ctx;

	if (fdr.verbose > 1)
		fprintf(stderr, "SIGHUP received\n");
	fdr.got_sighup = 1;
}

static int
fdr_open_log(const char *path)
{
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0400);
	if (fd < 0) {
		perror(path);
		exit(FDR_EC_OPENLOG);
	}
	return fd;
}

static int
fdr_throttle(int *counter)
{
	return (((*counter)++ % 1000) == 0) ? 0 : 1;
}

static void
fdr_rotate_logs(struct fdr_instance *insp)
{
	struct stat st;
	char confpath[FDR_PATH_MAX];
	pid_t pid;
	int status;
	char *argv[] = { "logrotate", "-f", confpath, NULL };

	snprintf(confpath, sizeof(confpath), "/etc/logrotate.d/%s", insp->iname);
	if (fdr.verbose > 1)
		fprintf(stderr, "looking for %s\n", confpath);
	if (stat(confpath, &st) != 0)
		return;

	pid = fork();
	if (pid == 0) {
		execv("/usr/sbin/logrotate", argv);
		perror("cannot exec logrotate");
		_exit(FDR_EC_EXEC);
	}
	if (pid == -1) {
		perror("fork");
		return;
	}
	if (waitpid(pid, &status, 0) == pid &&
	    !(WIFEXITED(status) && WEXITSTATUS(status) == 0))
		fprintf(stderr, "logrotate failed %d\n", status);
}

void
fdr_harvest_run(struct fdr_instance *insp, struct fdr_item *item)
{
	char tracepipe[FDR_PATH_MAX];
	int readfd, writefd, n, blocksize, pctfree, rotated = 0;
	char *buffer;
	struct stat st;
	struct statvfs vfs;
	struct sigaction sa;
	int warn_size = 0, warn_space = 0, warn_write = 0;

	if (stat(item->target, &st) == 0 && st.st_size > 0) {
		if (fdr.verbose)
			fprintf(stderr, "rotating %s\n", item->target);
		fdr_rotate_logs(insp);
	}

	snprintf(tracepipe, sizeof(tracepipe), "%s/%s/trace_pipe",
	    fdr.inst_dir, insp->iname);
	fprintf(stderr, "saving from %s to %s\n", tracepipe, item->target);

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = fdr_sighup_worker;
	if (sigaction(SIGHUP, &sa, NULL))
		perror("sigaction SIGHUP, continuing");

	readfd = open(tracepipe, O_RDONLY);
	if (readfd < 0) {
		perror(tracepipe);
		exit(FDR_EC_OPENTRACE);
	}
	if (fstat(readfd, &st) != 0) {
		perror(tracepipe);
		close(readfd);
		exit(FDR_EC_FSTAT);
	}

	blocksize = (int)st.st_blksize;
	if (blocksize <= 0)
		blocksize = 4096;

	buffer = malloc((size_t)blocksize);
	if (buffer == NULL) {
		perror("malloc");
		close(readfd);
		exit(FDR_EC_MALLOC);
	}

	insp->trace_fd = readfd;
	writefd = fdr_open_log(item->target);

	for (;;) {
		n = (int)read(readfd, buffer, (size_t)blocksize);

		if (n == -1 && fdr.got_sighup) {
			fdr.got_sighup = 0;
			close(writefd);
			writefd = fdr_open_log(item->target);
			continue;
		}
		if (n <= 0)
			break;

		if (fstat(writefd, &st) == 0) {
			if (st.st_nlink == 0) {
				fprintf(stderr, "closing %s\n", item->target);
				close(writefd);
				writefd = fdr_open_log(item->target);
			} else if (st.st_size > insp->maxsize) {
				if (!fdr_throttle(&warn_size))
					fprintf(stderr,
					    "file size for %s exceeded, rotating\n",
					    item->target);
				close(writefd);
				fdr_rotate_logs(insp);
				writefd = fdr_open_log(item->target);
			}
		}

		if (fstatvfs(writefd, &vfs) == 0) {
			pctfree = (int)((vfs.f_bavail * 100) / vfs.f_blocks);
			if (pctfree <= insp->minfree) {
				if (!fdr_throttle(&warn_space))
					fprintf(stderr,
					    "free space too low for %s\n",
					    item->target);
				if (rotated == 0) {
					close(writefd);
					fdr_rotate_logs(insp);
					writefd = fdr_open_log(item->target);
					rotated = 1;
				}
			} else {
				rotated = 0;
			}
		}

		if (write(writefd, buffer, (size_t)n) != n &&
		    !fdr_throttle(&warn_write))
			perror(item->target);
	}

	close(writefd);
	close(readfd);
	free(buffer);
	insp->trace_fd = -1;
}
