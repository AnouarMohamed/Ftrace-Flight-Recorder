/*
 * process.c - worker processes, safe signal handling, and reloads
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
fdr_parent_signal(int signo)
{
	if (signo == SIGCHLD)
		fdr.got_sigchld = 1;
	else if (signo == SIGHUP)
		fdr.want_reload = 1;
	else
		fdr.want_exit = 1;
}

void
fdr_process_install_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fdr_parent_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP;
	if (sigaction(SIGTERM, &sa, NULL) != 0 ||
	    sigaction(SIGINT, &sa, NULL) != 0 ||
	    sigaction(SIGHUP, &sa, NULL) != 0 ||
	    sigaction(SIGCHLD, &sa, NULL) != 0)
		fdr_die(FDR_EC_SYSTEM, "cannot install signal handlers: %s",
		    strerror(errno));
	sa.sa_handler = SIG_IGN;
	if (sigaction(SIGUSR1, &sa, NULL) != 0)
		fdr_die(FDR_EC_SYSTEM, "cannot ignore SIGUSR1: %s",
		    strerror(errno));
}

static void
fdr_worker_reset_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGTERM, &sa, NULL) != 0 ||
	    sigaction(SIGINT, &sa, NULL) != 0 ||
	    sigaction(SIGCHLD, &sa, NULL) != 0)
		_exit(FDR_EC_SYSTEM);
	sa.sa_handler = SIG_IGN;
	if (sigaction(SIGHUP, &sa, NULL) != 0 ||
	    sigaction(SIGUSR1, &sa, NULL) != 0)
		_exit(FDR_EC_SYSTEM);
}

static int
fdr_run_instance(struct fdr_instance *insp)
{
	const struct fdr_item *item;
	const struct fdr_item *saveto = NULL;
	int probe_errors = 0;

	for (item = insp->items; item != NULL; item = item->next) {
		switch (item->type) {
		case FDR_ITEM_INSTANCE:
			if (fdr_trace_create_instance(insp, item) != 0)
				return FDR_EC_MKDIR;
			break;
		case FDR_ITEM_MODPROBE:
			if (fdr_trace_load_module(item) != 0)
				return FDR_EC_SYSTEM;
			break;
		case FDR_ITEM_ENABLE:
		case FDR_ITEM_DISABLE:
			if (fdr_trace_set_probe(insp, item) != 0)
				probe_errors++;
			break;
		case FDR_ITEM_SAVETO:
			saveto = item;
			break;
		case FDR_ITEM_MINFREE:
			break;
		}
	}

	if (probe_errors != 0)
		fdr_warn("instance %s started with %d probe error(s)",
		    insp->iname, probe_errors);
	if (saveto == NULL)
		return 0;
	return fdr_harvest_run(insp, saveto) == 0 ? 0 : FDR_EC_WRITE;
}

static int
fdr_record_child(pid_t pid, struct fdr_instance *insp)
{
	int slot;

	for (slot = 0; slot < FDR_MAX_CHILDREN; slot++) {
		if (fdr.child_pids[slot] == 0)
			break;
	}
	if (slot == FDR_MAX_CHILDREN)
		return -1;
	fdr.child_pids[slot] = pid;
	fdr.child_persistent[slot] = insp->has_saveto;
	fdr.child_instances[slot] = insp;
	fdr.num_children++;
	if (fdr.metrics != NULL)
		fdr_metrics_store_int(&fdr.metrics->workers_alive,
		    fdr.num_children);
	return 0;
}

static int
fdr_spawn_instance(struct fdr_instance *insp)
{
	pid_t pid;

	fflush(stdout);
	fflush(stderr);
	pid = fork();
	if (pid < 0) {
		fdr_warn("cannot fork worker for %s: %s", insp->iname,
		    strerror(errno));
		return -1;
	}
	if (pid == 0) {
		int rc;

		fdr_worker_reset_signals();
		rc = fdr_run_instance(insp);
		fdr_log(rc == 0 ? "info" : "error", "instance %s exited with status %d",
		    insp->iname, rc);
		_exit(rc);
	}
	if (fdr_record_child(pid, insp) != 0) {
		(void)kill(pid, SIGTERM);
		(void)waitpid(pid, NULL, 0);
		fdr_warn("too many workers");
		return -1;
	}
	fdr_log("info", "started worker %d for instance %s", (int)pid,
	    insp->iname);
	return 0;
}

int
fdr_process_start_all(void)
{
	struct fdr_instance *insp;

	for (insp = fdr.instances; insp != NULL; insp = insp->next) {
		if (fdr_spawn_instance(insp) != 0) {
			fdr_process_stop_children();
			return -1;
		}
	}
	return 0;
}

static void
fdr_clear_child_slot(int slot)
{
	fdr.child_pids[slot] = 0;
	fdr.child_persistent[slot] = 0;
	fdr.child_instances[slot] = NULL;
	if (fdr.num_children > 0)
		fdr.num_children--;
	if (fdr.metrics != NULL)
		fdr_metrics_store_int(&fdr.metrics->workers_alive,
		    fdr.num_children);
}

void
fdr_process_reap_children(void)
{
	pid_t pid;
	int status;

	fdr.got_sigchld = 0;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		int slot;
		int persistent = 0;
		const char *name = "unknown";
		int success = WIFEXITED(status) && WEXITSTATUS(status) == 0;

		for (slot = 0; slot < FDR_MAX_CHILDREN; slot++) {
			if (fdr.child_pids[slot] == pid) {
				persistent = fdr.child_persistent[slot];
				if (fdr.child_instances[slot] != NULL)
					name = fdr.child_instances[slot]->iname;
				fdr_clear_child_slot(slot);
				break;
			}
		}

		if (fdr.want_exit || fdr.reloading)
			continue;
		if (!success || persistent) {
			if (WIFEXITED(status))
				fdr_log("error",
				    "worker %d for %s exited unexpectedly with status %d",
				    (int)pid, name, WEXITSTATUS(status));
			else
				fdr_log("error",
				    "worker %d for %s terminated unexpectedly",
				    (int)pid, name);
			if (fdr.metrics != NULL)
				fdr_metrics_store_int(&fdr.metrics->healthy, 0);
			fdr.exit_status = 1;
			fdr.want_exit = 1;
		} else if (fdr.verbose) {
			fdr_log("info", "setup-only worker %d for %s completed",
			    (int)pid, name);
		}
	}
}

void
fdr_process_stop_children(void)
{
	int slot;

	for (slot = 0; slot < FDR_MAX_CHILDREN; slot++) {
		if (fdr.child_pids[slot] > 0 &&
		    kill(fdr.child_pids[slot], SIGTERM) != 0 && errno != ESRCH)
			fdr_warn("cannot stop worker %d: %s",
			    (int)fdr.child_pids[slot], strerror(errno));
	}
	for (slot = 0; slot < FDR_MAX_CHILDREN; slot++) {
		pid_t pid = fdr.child_pids[slot];

		if (pid <= 0)
			continue;
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
		fdr_clear_child_slot(slot);
	}
	fdr.got_sigchld = 0;
}

void
fdr_process_cleanup_instances(void)
{
	struct fdr_instance *insp;

	for (insp = fdr.instances; insp != NULL; insp = insp->next) {
		if (insp->dname[0] != '\0' && rmdir(insp->dname) != 0 &&
		    errno != ENOENT)
			fdr_warn("cannot remove trace instance %s: %s",
			    insp->dname, strerror(errno));
	}
}

int
fdr_process_reload(void)
{
	struct fdr_instance *old_instances = fdr.instances;
	int old_count = fdr.instance_count;
	struct fdr_instance *new_instances;
	int new_count;

	fdr.instances = NULL;
	fdr.instance_count = 0;
	if (fdr_config_load(fdr.config_dir) != 0) {
		fdr_config_free();
		fdr.instances = old_instances;
		fdr.instance_count = old_count;
		return -1;
	}
	new_instances = fdr.instances;
	new_count = fdr.instance_count;
	fdr.instances = old_instances;
	fdr.instance_count = old_count;

	fdr.reloading = 1;
	fdr_process_stop_children();
	fdr_process_cleanup_instances();
	fdr_config_free();
	fdr.instances = new_instances;
	fdr.instance_count = new_count;
	if (fdr.metrics != NULL) {
		fdr_metrics_store_int(&fdr.metrics->instances, new_count);
		fdr_metrics_store_int(&fdr.metrics->healthy, 1);
	}
	if (fdr_process_start_all() != 0) {
		fdr.reloading = 0;
		fdr.exit_status = 1;
		fdr.want_exit = 1;
		return -1;
	}
	fdr.reloading = 0;
	if (fdr.metrics != NULL)
		fdr_metrics_add(&fdr.metrics->reloads, 1);
	fdr_log("info", "configuration reload completed");
	return 0;
}
