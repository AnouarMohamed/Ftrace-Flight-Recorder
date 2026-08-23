/*
 * fdr.h - Flight Data Recorder shared definitions
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 */

#ifndef FDR_H
#define FDR_H

#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#define FDR_BUFSIZE		256
#define FDR_PATH_MAX		4096
#define FDR_CONFIG_DIR		"/etc/fdr.d"
#define FDR_TRACE_INST_DIR	"/sys/kernel/tracing/instances"
#define FDR_DEBUG_INST_DIR	"/sys/kernel/debug/tracing/instances"
#define FDR_TRACE_EVENTS_DIR	"/sys/kernel/tracing/events"
#define FDR_DEBUG_EVENTS_DIR	"/sys/kernel/debug/tracing/events"
#define FDR_MAX_CHILDREN	64
#define FDR_HTTP_PORT_DEFAULT	9119

#define FDR_MINFREE_DEFAULT	5
#define FDR_MAXSIZE_DEFAULT	INT_MAX

#define FDR_EC_MKDIR		1
#define FDR_EC_SYSTEM		2
#define FDR_EC_SYNTAX		3
#define FDR_EC_BADTYPE1		4
#define FDR_EC_OPEN		5
#define FDR_EC_WRITE1		6
#define FDR_EC_OPENLOG		7
#define FDR_EC_OPENTRACE	8
#define FDR_EC_FSTAT		9
#define FDR_EC_MALLOC		10
#define FDR_EC_BADVERB		11
#define FDR_EC_FORK		12
#define FDR_EC_BADTYPE2		13
#define FDR_EC_BADARGS		14
#define FDR_EC_EXEC		15
#define FDR_EC_CONFIG		16
#define FDR_EC_HTTP		17

enum fdr_item_type {
	FDR_ITEM_INSTANCE = 0,
	FDR_ITEM_MODPROBE,
	FDR_ITEM_ENABLE,
	FDR_ITEM_DISABLE,
	FDR_ITEM_SAVETO,
	FDR_ITEM_LOGROT,
	FDR_ITEM_MINFREE,
	FDR_ITEM_RATELIMIT,
};

struct fdr_item {
	struct fdr_item	*next;
	enum fdr_item_type type;
	char		verb[FDR_BUFSIZE];
	char		target[FDR_BUFSIZE];
	char		fpath[FDR_BUFSIZE];
	char		optarg[FDR_BUFSIZE];
	int		line;
};

struct fdr_instance {
	struct fdr_instance *next;
	struct fdr_item	*items;
	char		iname[FDR_BUFSIZE];
	char		dname[FDR_PATH_MAX];
	int		trace_fd;
	unsigned long	bufsize;
	long		maxsize;
	int		minfree;
	unsigned long	ratelimit_bps;	/* 0 = unlimited */
};

/* Shared across parent and workers (mmap MAP_SHARED). */
struct fdr_metrics {
	volatile uint64_t bytes_written;
	volatile uint64_t bytes_dropped;
	volatile uint64_t rotations;
	volatile uint64_t probe_failures;
	volatile uint64_t rate_limit_drops;
	volatile uint64_t write_errors;
	volatile uint64_t reloads;
	volatile int	  instances;
	volatile int	  workers_alive;
	volatile int	  healthy;
};

struct fdr_runtime {
	int		verbose;
	int		json_log;
	volatile sig_atomic_t got_sighup;
	volatile sig_atomic_t want_reload;
	volatile sig_atomic_t want_exit;
	int		parse_only;
	int		list_probes;
	int		foreground;
	int		http_port;
	int		num_children;
	pid_t		child_pids[FDR_MAX_CHILDREN];
	char		inst_dir[FDR_PATH_MAX];
	char		config_dir[FDR_PATH_MAX];
	struct fdr_instance *instances;
	int		instance_count;
	struct fdr_metrics *metrics;
};

extern struct fdr_runtime fdr;

void fdr_die(int code, const char *fmt, ...);
void fdr_log(const char *level, const char *fmt, ...);
void fdr_warn(const char *fmt, ...);

void fdr_copy_field(char *dst, size_t dstsz, const char *src);
int fdr_join_path(char *dst, size_t dstsz, const char *a, const char *b);
void fdr_chomp_line(char *buf);
unsigned long fdr_parse_size(const char *arg);
const char *fdr_default_inst_dir(void);
const char *fdr_default_events_dir(void);

void fdr_metrics_init(void);
void fdr_metrics_add(volatile uint64_t *counter, uint64_t delta);

void fdr_instance_init(struct fdr_instance *insp);
void fdr_instance_append(struct fdr_instance *insp);
void fdr_config_free(void);
int fdr_config_load(const char *dir);
/* Test helper: parse one config line into an instance. Returns 0/-1. */
int fdr_config_parse_line_test(struct fdr_instance *insp, const char *line);

void fdr_trace_create_instance(struct fdr_instance *insp, struct fdr_item *item);
void fdr_trace_load_module(struct fdr_item *item);
void fdr_trace_set_probe(struct fdr_instance *insp, struct fdr_item *item);
int fdr_trace_list_probes(void);

void fdr_harvest_run(struct fdr_instance *insp, struct fdr_item *item);

void fdr_process_start_all(void);
void fdr_process_stop_children(void);
void fdr_process_install_handlers(void);
void fdr_process_reap_children(void);
int fdr_process_reload(void);

int fdr_http_serve(int port);

#endif /* FDR_H */
