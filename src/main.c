/*
 * main.c - entry point and CLI
 */

#include "fdr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-fnv] [-c config-dir] [-d trace-root]\n",
	    prog);
	fprintf(stderr, "  -f  run in foreground (required for containers)\n");
	fprintf(stderr, "  -n  parse configuration and exit (no daemon)\n");
	fprintf(stderr, "  -v  increase verbosity\n");
}

int
main(int argc, char **argv)
{
	int opt, ret;

	fdr_copy_field(fdr.config_dir, sizeof(fdr.config_dir), FDR_CONFIG_DIR);
	fdr_copy_field(fdr.inst_dir, sizeof(fdr.inst_dir),
	    fdr_default_inst_dir());

	while ((opt = getopt(argc, argv, "fnvc:d:")) != -1) {
		switch (opt) {
		case 'f':
			fdr.foreground = 1;
			break;
		case 'n':
			fdr.parse_only = 1;
			break;
		case 'v':
			fdr.verbose++;
			break;
		case 'c':
			fdr_copy_field(fdr.config_dir, sizeof(fdr.config_dir),
			    optarg);
			break;
		case 'd':
			snprintf(fdr.inst_dir, sizeof(fdr.inst_dir),
			    "%s/instances", optarg);
			break;
		default:
			usage(argv[0]);
			exit(FDR_EC_BADARGS);
		}
	}

	if (!fdr.parse_only && !fdr.foreground) {
		ret = daemon(1, 1);
		if (ret != 0)
			return ret;
	}

	fdr_process_install_handlers();

	if (fdr_config_load(fdr.config_dir) != 0)
		exit(FDR_EC_CONFIG);

	fdr_process_start_all();

	if (fdr.parse_only)
		exit(0);

	for (;;)
		pause();
}
