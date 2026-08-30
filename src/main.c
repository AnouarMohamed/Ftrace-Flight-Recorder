/*
 * main.c - Flight Data Recorder entry point, CLI parser, and process orchestrator
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Architecture Overview:
 * `main()` coordinates the full lifecycle of the FDR supervisor daemon:
 * 1. CLI Parsing & Validation: Parses command-line arguments using getopt(3),
 *    validating port ranges, path lengths, verbosity levels, and execution modes.
 * 2. Configuration Loading & Dry-Run: Loads `/etc/fdr.d` configuration files. In parse-only mode (-n),
 *    validates syntax and exits 0 without initializing kernel tracefs state.
 * 3. Daemonization: If `-f` (foreground) is omitted, detaches into background via `daemon(1,1)`.
 * 4. IPC & Metrics Setup: Allocates the anonymous shared memory page (`fdr_metrics_init`),
 *    stores active instance count, and installs async-signal-safe supervisor handlers.
 * 5. Worker Dispatch: Spawns child worker processes for every loaded instance.
 * 6. Event Multiplexing: Runs `fdr_http_serve()`, serving HTTP endpoints and coordinating
 *    signal-driven tasks (reaping, reloads, loss sampling).
 * 7. Graceful Teardown: Stops workers with SIGTERM, removes tracefs instance directories,
 *    frees configuration memory, and unmaps shared metrics.
 */

#include "fdr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef FDR_VERSION
#define FDR_VERSION "development"
#endif

/**
 * usage - Prints command-line syntax and option descriptions to a stream.
 *
 * @stream: Target output stream (e.g. stdout or stderr).
 * @prog: Program name string (argv[0]).
 */
static void
usage(FILE *stream, const char *prog)
{
	fprintf(stream,
	    "usage: %s [-fjnv] [-a address] [-p port] "
	    "[-c config-dir] [-d trace-root]\n"
	    "  -a  HTTP listen address (default " FDR_HTTP_ADDR_DEFAULT ")\n"
	    "  -c  configuration directory (default " FDR_CONFIG_DIR ")\n"
	    "  -d  tracefs root (the instances directory is appended)\n"
	    "  -f  run in foreground\n"
	    "  -j  emit JSON logs\n"
	    "  -n  validate configuration and exit\n"
	    "  -p  HTTP port; 0 disables HTTP (default %d)\n"
	    "  -v  increase verbosity\n"
	    "  -V  print version and exit\n",
	    prog, FDR_HTTP_PORT_DEFAULT);
}

/**
 * parse_port - Validates and parses a TCP port number string.
 *
 * Uses strtol(3) to parse the integer and enforces the valid port range [0, 65535].
 * Port 0 is a special valid value that explicitly disables the HTTP server.
 *
 * @text: Null-terminated numeric port string.
 * Return: Parsed port integer >= 0, or -1 on invalid syntax or out-of-range value.
 */
static int
parse_port(const char *text)
{
	char *end;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno != 0 || *text == '\0' || *end != '\0' ||
	    value < 0 || value > 65535)
		return -1;
	return (int)value;
}

/**
 * main - Main supervisor entry point.
 *
 * @argc: Number of command-line arguments.
 * @argv: Array of argument strings.
 * Return: 0 on clean exit, or non-zero FDR_EC_* exit code on error.
 */
int
main(int argc, char **argv)
{
	int opt;
	int rc;

	/* Initialize runtime state defaults */
	if (fdr_copy_field(fdr.config_dir, sizeof(fdr.config_dir),
	    FDR_CONFIG_DIR) != 0 ||
	    fdr_copy_field(fdr.inst_dir, sizeof(fdr.inst_dir),
	    fdr_default_inst_dir()) != 0 ||
	    fdr_copy_field(fdr.http_addr, sizeof(fdr.http_addr),
	    FDR_HTTP_ADDR_DEFAULT) != 0)
		return FDR_EC_BADARGS;
	fdr.http_port = FDR_HTTP_PORT_DEFAULT;

	/* Parse CLI option flags */
	while ((opt = getopt(argc, argv, "a:c:d:fjnp:vV")) != -1) {
		switch (opt) {
		case 'a':
			if (fdr_copy_field(fdr.http_addr, sizeof(fdr.http_addr),
			    optarg) != 0)
				fdr_die(FDR_EC_BADARGS, "HTTP address is too long");
			break;
		case 'c':
			if (fdr_copy_field(fdr.config_dir, sizeof(fdr.config_dir),
			    optarg) != 0)
				fdr_die(FDR_EC_BADARGS,
				    "configuration path is too long");
			break;
		case 'd':
			if (fdr_join_path(fdr.inst_dir, sizeof(fdr.inst_dir),
			    optarg, "instances") != 0)
				fdr_die(FDR_EC_BADARGS, "tracefs path is too long");
			break;
		case 'f':
			fdr.foreground = 1;
			break;
		case 'j':
			fdr.json_log = 1;
			break;
		case 'n':
			fdr.parse_only = 1;
			break;
		case 'p':
			fdr.http_port = parse_port(optarg);
			if (fdr.http_port < 0)
				fdr_die(FDR_EC_BADARGS, "invalid HTTP port: %s",
				    optarg);
			break;
		case 'v':
			fdr.verbose++;
			break;
		case 'V':
			printf("fdrd %s\n", FDR_VERSION);
			return 0;
		default:
			usage(stderr, argv[0]);
			return FDR_EC_BADARGS;
		}
	}
	if (optind != argc) {
		usage(stderr, argv[0]);
		return FDR_EC_BADARGS;
	}

	/* Load and validate configuration files */
	if (fdr_config_load(fdr.config_dir) != 0) {
		fdr_config_free();
		return FDR_EC_CONFIG;
	}

	/* If -n flag provided, exit immediately after validation */
	if (fdr.parse_only) {
		fdr_log("info", "validated %d instance configuration(s)",
		    fdr.instance_count);
		fdr_config_free();
		return 0;
	}

	/* Detach into background daemon unless -f was specified */
	if (!fdr.foreground && daemon(1, 1) != 0) {
		fdr_warn("cannot daemonize: %s", strerror(errno));
		fdr_config_free();
		return FDR_EC_SYSTEM;
	}

	/* Allocate shared metrics, install supervisor signals, and spawn workers */
	fdr_metrics_init();
	fdr_metrics_store_int(&fdr.metrics->instances, fdr.instance_count);
	fdr_process_install_handlers();
	if (fdr_process_start_all() != 0) {
		fdr_process_cleanup_instances();
		fdr_config_free();
		fdr_metrics_destroy();
		return FDR_EC_FORK;
	}

	/* Run parent supervisor multiplexer and HTTP server */
	rc = fdr_http_serve(fdr.http_addr, fdr.http_port);

	/* Graceful termination cleanup sequence */
	fdr.want_exit = 1;
	fdr_process_stop_children();
	fdr_process_cleanup_instances();
	fdr_config_free();
	fdr_metrics_destroy();
	fdr_log(rc == 0 ? "info" : "error", "fdrd stopped with status %d", rc);
	return rc;
}

