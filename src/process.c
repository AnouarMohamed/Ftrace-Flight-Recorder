/*
 * process.c - worker processes, signals, and shutdown
 */

#include "fdr.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void
fdr_remove_instance_dir(struct fdr_instance *insp)
{
	if (insp->dname[0] != '\0' && rmdir(insp->dname) != 0 && errno != ENOENT)
		fprintf(stderr, "rmdir %s failed, errno %d\n",
		    insp->dname, errno);
}

static void
fdr_cleanup_instances(void)
{
	struct fdr_instance *insp;

	for (insp = fdr.instances; insp != NULL; insp = insp->next)
		fdr_remove_instance_dir(insp);
}

static void
fdr_record_child(pid_t pid)
{
	if (fdr.num_children >= FDR_MAX_CHILDREN) {
		fprintf(stderr, "too many instance workers (%d max)\n",
		    FDR_MAX_CHILDREN);
		exit(FDR_EC_FORK);
	}
	fdr.child_pids[fdr.num_children++] = pid;
}

static void
fdr_stop_children(void)
{
	int i;

	for (i = 0; i < fdr.num_children; i++) {
		if (fdr.child_pids[i] > 0)
			kill(fdr.child_pids[i], SIGTERM);
	}
	for (i = 0; i < fdr.num_children; i++) {
		if (fdr.child_pids[i] > 0)
			waitpid(fdr.child_pids[i], NULL, 0);
	}
	fdr.num_children = 0;
}

static void
fdr_shutdown(int signo, siginfo_t *info, void *ctx)
{
	(void)info;
	(void)ctx;

	if (fdr.verbose > 1)
		fprintf(stderr, "signal %d received, cleaning up\n", signo);

	fdr_stop_children();
	fdr_cleanup_instances();
	_exit(0);
}

static void
fdr_sigchld(int signo, siginfo_t *info, void *ctx)
{
	(void)signo;
	(void)info;
	(void)ctx;

	fdr_process_reap_children();
}

void
fdr_process_reap_children(void)
{
	pid_t pid;
	int status;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (fdr.verbose > 1)
			fprintf(stderr, "reaped child %d\n", (int)pid);
	}
}

void
fdr_process_install_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = fdr_shutdown;
	if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL))
		perror("sigaction");

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = fdr_sigchld;
	sa.sa_flags = SA_NOCLDSTOP;
	if (sigaction(SIGCHLD, &sa, NULL))
		perror("sigaction SIGCHLD");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	if (sigaction(SIGHUP, &sa, NULL))
		perror("sigaction SIGHUP");
}

static void
fdr_run_instance(struct fdr_instance *insp)
{
	struct fdr_item *item;
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL))
		perror("sigaction");

	for (item = insp->items; item != NULL; item = item->next) {
		switch (item->type) {
		case FDR_ITEM_INSTANCE:
			fdr_trace_create_instance(insp, item);
			break;
		case FDR_ITEM_MODPROBE:
			fdr_trace_load_module(item);
			break;
		case FDR_ITEM_ENABLE:
		case FDR_ITEM_DISABLE:
			fdr_trace_set_probe(insp, item);
			break;
		case FDR_ITEM_SAVETO:
			fdr_harvest_run(insp, item);
			break;
		case FDR_ITEM_MINFREE:
			break;
		default:
			fprintf(stderr, "internal error, bad item type %d\n",
			    item->type);
			_exit(FDR_EC_BADTYPE1);
		}
	}

	fprintf(stderr, "instance %s exiting\n", insp->iname);
	_exit(0);
}

static void
fdr_spawn_instance(struct fdr_instance *insp)
{
	pid_t pid;

	if (fdr.parse_only) {
		fprintf(stderr, "validated instance %s (%s)\n",
		    insp->iname, insp->dname);
		return;
	}

	if (fdr.verbose > 1)
		fprintf(stderr, "creating instance for %s\n", insp->iname);

	fflush(stdout);
	fflush(stderr);

	pid = fork();
	if (pid == (pid_t)-1) {
		perror("fork");
		exit(FDR_EC_FORK);
	}
	if (pid != 0) {
		fdr_record_child(pid);
		return;
	}

	fdr_run_instance(insp);
}

void
fdr_process_start_all(void)
{
	struct fdr_instance *insp;

	for (insp = fdr.instances; insp != NULL; insp = insp->next)
		fdr_spawn_instance(insp);
}
