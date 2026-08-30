/*
 * sched_load.c - Multi-threaded synthetic scheduler load generator for trace benchmark testing
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Purpose:
 * Generates controlled high-frequency scheduler context switches (`sched/sched_switch` events)
 * to stress test FDR capture workers and tracefs ring buffers:
 * - Spawns N worker threads executing tight `sched_yield(2)` loops in bursts.
 * - Pauses worker threads between bursts with `nanosleep`.
 * - Measures total aggregate yields across all threads over a fixed run duration.
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/**
 * struct worker_state - Thread-local workload generator state.
 * @stop: Shared atomic termination flag.
 * @pause: Sleep interval between yield bursts.
 * @burst: Number of consecutive sched_yield() calls per burst.
 * @yields: Local counter tracking successful yields performed.
 */
struct worker_state {
	atomic_int *stop;
	struct timespec pause;
	unsigned int burst;
	uint64_t yields;
};

/**
 * positive_arg - Validates and parses a positive integer CLI argument.
 *
 * @text: Numeric string.
 * Return: Parsed unsigned integer, or 0 if out of range [1, 3600].
 */
static unsigned int
positive_arg(const char *text)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value == 0 ||
	    value > 3600)
		return 0;
	return (unsigned int)value;
}

/**
 * run_worker - Worker thread loop executing bursts of sched_yield().
 *
 * @opaque: Pointer to struct worker_state.
 * Return: Always returns NULL upon termination.
 */
static void *
run_worker(void *opaque)
{
	struct worker_state *state = opaque;

	while (!atomic_load_explicit(state->stop, memory_order_relaxed)) {
		struct timespec remaining = state->pause;
		unsigned int i;

		for (i = 0; i < state->burst &&
		    !atomic_load_explicit(state->stop, memory_order_relaxed); i++) {
			(void)sched_yield();
			state->yields++;
		}
		while (nanosleep(&remaining, &remaining) != 0 &&
		    errno == EINTR)
			;
	}
	return NULL;
}

int
main(int argc, char **argv)
{
	struct timespec remaining;
	pthread_t *threads;
	struct worker_state *states;
	atomic_int stop = 0;
	uint64_t total = 0;
	unsigned int seconds;
	unsigned int workers;
	unsigned int pause_us;
	unsigned int burst;
	unsigned int i;

	if ((argc < 3 || argc > 5) ||
	    (seconds = positive_arg(argv[1])) == 0 ||
	    (workers = positive_arg(argv[2])) == 0 || workers > 4096) {
		fprintf(stderr,
		    "usage: %s seconds workers [pause-us [yield-burst]]\n", argv[0]);
		return 2;
	}
	pause_us = argc >= 4 ? positive_arg(argv[3]) : 50;
	burst = argc == 5 ? positive_arg(argv[4]) : 16;
	if (pause_us == 0) {
		fprintf(stderr, "pause-us must be between 1 and 3600\n");
		return 2;
	}
	if (burst == 0) {
		fprintf(stderr, "yield-burst must be between 1 and 3600\n");
		return 2;
	}

	threads = calloc(workers, sizeof(*threads));
	states = calloc(workers, sizeof(*states));
	if (threads == NULL || states == NULL) {
		free(threads);
		free(states);
		return 1;
	}

	/* Launch all worker threads */
	for (i = 0; i < workers; i++) {
		states[i].stop = &stop;
		states[i].pause.tv_sec = pause_us / 1000000;
		states[i].pause.tv_nsec = (long)(pause_us % 1000000) * 1000L;
		states[i].burst = burst;
		if (pthread_create(&threads[i], NULL, run_worker, &states[i]) != 0) {
			atomic_store_explicit(&stop, 1, memory_order_relaxed);
			while (i > 0)
				(void)pthread_join(threads[--i], NULL);
			free(states);
			free(threads);
			return 1;
		}
	}

	/* Sleep for the requested duration */
	remaining.tv_sec = (time_t)seconds;
	remaining.tv_nsec = 0;
	while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR)
		;

	/* Signal all workers to stop and wait for completion */
	atomic_store_explicit(&stop, 1, memory_order_relaxed);
	for (i = 0; i < workers; i++) {
		(void)pthread_join(threads[i], NULL);
		total += states[i].yields;
	}

	printf("workers=%u seconds=%u pause_us=%u yield_burst=%u "
	    "yields=%" PRIu64 "\n", workers, seconds, pause_us, burst, total);

	free(states);
	free(threads);
	return 0;
}

