/*
 * process.c - Worker process supervision, signal dispatching, and transactional reloads
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Overview:
 * This module manages the multi-process supervisor lifecycle:
 * 1. Signal Architecture: Installs async-signal-safe parent signal handlers using
 *    `sigaction(2)` with `SA_NOCLDSTOP`. Signal handlers only set atomic volatile
 *    flags (`fdr.got_sigchld`, `fdr.want_reload`, `fdr.want_exit`), deferring
 *    complex handling to the main event loop.
 * 2. Worker Spawning & Isolation: Forks isolated worker processes for each tracefs
 *    instance. Workers reset signal masks and execute `fdr_run_instance()`.
 * 3. Child Process Supervision: The supervisor maintains a process table (`child_pids`)
 *    and asynchronously reaps dead children (`fdr_process_reap_children` via `waitpid(WNOHANG)`).
 *    If a persistent `saveto` worker dies unexpectedly, the supervisor shuts down
 *    so systemd or Kubernetes can reconstruct a consistent instance state.
 * 4. Transactional SIGHUP Reloads: Implements atomic zero-downtime configuration
 *    validation. A new configuration is parsed completely before stopping old workers;
 *    invalid syntax is rejected without disturbing the running system.
 */

#include "fdr.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/**
 * fdr_parent_signal - Async-signal-safe parent signal handler.
 *
 * Updates sig_atomic_t flags without performing non-reentrant operations:
 * - SIGCHLD: Sets `got_sigchld` to trigger non-blocking reaping in the event loop.
 * - SIGHUP: Sets `want_reload` to trigger transactional configuration reload.
 * - SIGTERM / SIGINT: Sets `want_exit` to trigger graceful daemon shutdown.
 *
 * @signo: Signal number received.
 */
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

/**
 * fdr_process_install_handlers - Installs supervisor signal dispositions via sigaction.
 *
 * Sets up handlers with `SA_NOCLDSTOP` (so SIGCHLD is generated only when children
 * terminate, not when stopped or resumed). Ignores SIGUSR1 in parent so it is handled
 * exclusively by workers.
 */
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

/**
 * fdr_worker_reset_signals - Resets signal dispositions in child worker processes after fork.
 *
 * Restores default termination dispositions for SIGTERM, SIGINT, SIGCHLD, and ignores
 * SIGHUP and SIGUSR1 until the harvest loop sets its own handlers.
 */
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

/**
 * fdr_run_instance - Worker process entry point: executes instance configuration AST.
 *
 * Iterates through `insp->items`:
 * 1. Creates tracefs instance and sets per-CPU buffer size (`FDR_ITEM_INSTANCE`).
 * 2. Loads kernel modules if required (`FDR_ITEM_MODPROBE`).
 * 3. Configures filters and enables/disables tracepoints (`FDR_ITEM_ENABLE` / `FDR_ITEM_DISABLE`).
 * 4. If `saveto` is configured, enters continuous harvest loop (`fdr_harvest_run`).
 *    If `saveto` is omitted, exits cleanly as a setup-only instance.
 *
 * @insp: Instance configuration structure.
 * Return: 0 on success, or non-zero FDR_EC_* exit code on error.
 */
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

	/* Setup-only instance: exit cleanly without starting a stream collector */
	if (saveto == NULL)
		return 0;

	return fdr_harvest_run(insp, saveto) == 0 ? 0 : FDR_EC_WRITE;
}

/**
 * fdr_record_child - Registers an active child worker in the supervisor process table.
 *
 * Finds an empty slot in `fdr.child_pids`, records PID, persistent flag, and instance pointer,
 * and updates `fdr.metrics->workers_alive` gauge.
 *
 * @pid: Process ID of the newly forked child.
 * @insp: Associated instance configuration structure.
 * Return: 0 on success, or -1 if process table is full (FDR_MAX_CHILDREN).
 */
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

/**
 * fdr_spawn_instance - Forks a child worker process for a specific instance.
 *
 * Flushes standard streams before fork(2) to prevent duplicate output, resets
 * worker signals in child, and registers the worker in supervisor tables.
 *
 * @insp: Target instance structure.
 * Return: 0 on successful spawn, or -1 on fork/registration error.
 */
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

/**
 * fdr_process_start_all - Spawns worker processes for all loaded configuration instances.
 *
 * If any worker fails to spawn, immediately terminates previously spawned children
 * and returns -1.
 *
 * Return: 0 on complete success, -1 on failure.
 */
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

/**
 * fdr_clear_child_slot - Clears a process slot in the supervisor table.
 *
 * Decrements `fdr.num_children` and updates `fdr.metrics->workers_alive` gauge.
 *
 * @slot: Index in `fdr.child_pids` array.
 */
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

/**
 * fdr_process_reap_children - Asynchronously reaps exited child workers via waitpid(WNOHANG).
 *
 * Clears `fdr.got_sigchld` and repeatedly reaps finished children:
 * - If a setup-only worker exits with status 0, logs completion and updates state.
 * - If a persistent `saveto` worker terminates unexpectedly (crash, kill, or non-zero exit),
 *   logs an error, marks readiness as degraded (healthy = 0), and sets `want_exit = 1`
 *   so the supervisor daemon exits and allows orchestrator (systemd/K8s) recovery.
 */
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

		/* Ignore death during intentional reload or shutdown */
		if (fdr.want_exit || fdr.reloading)
			continue;

		/* Unexpected termination of persistent collector is a fatal supervisor event */
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

/**
 * fdr_process_stop_children - Sends SIGTERM to all child workers and waits for termination.
 *
 * Two-phase termination:
 * 1. Broadcasts SIGTERM to all active worker PIDs.
 * 2. Calls blocking waitpid(2) on each worker PID to guarantee clean exit.
 */
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

/**
 * fdr_process_cleanup_instances - Deletes all tracefs instance directories created by FDR.
 *
 * Invokes rmdir(2) on `/sys/kernel/tracing/instances/<name>` for each loaded instance.
 */
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

/**
 * fdr_process_reload - Transactionally reloads configuration on SIGHUP.
 *
 * Transactional guarantee:
 * 1. Parses new configuration from `/etc/fdr.d` into a temporary instance list.
 * 2. If parsing fails, the reload is aborted and existing workers continue running untouched.
 * 3. If parsing succeeds, stops existing workers, removes old tracefs instances,
 *    swaps in the new instance list, creates new instances, spawns new workers,
 *    and increments `fdr.metrics->reloads`.
 *
 * Return: 0 on successful reload, or -1 if new configuration was rejected.
 */
int
fdr_process_reload(void)
{
	struct fdr_instance *old_instances = fdr.instances;
	int old_count = fdr.instance_count;
	struct fdr_instance *new_instances;
	int new_count;

	/* Parse new configuration in isolated list */
	fdr.instances = NULL;
	fdr.instance_count = 0;
	if (fdr_config_load(fdr.config_dir) != 0) {
		fdr_config_free();
		/* Restore original configuration without disruption */
		fdr.instances = old_instances;
		fdr.instance_count = old_count;
		return -1;
	}

	new_instances = fdr.instances;
	new_count = fdr.instance_count;
	fdr.instances = old_instances;
	fdr.instance_count = old_count;

	/* Stop old workers and tear down instances */
	fdr.reloading = 1;
	fdr_process_stop_children();
	fdr_process_cleanup_instances();
	fdr_config_free();

	/* Activate new configuration instances */
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

