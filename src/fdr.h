/*
 * fdr.h - Flight Data Recorder shared definitions
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 */

#ifndef FDR_H
#define FDR_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define FDR_NAME_MAX             256
#define FDR_PATH_MAX             4096
#define FDR_CONFIG_LINE_MAX      4096
#define FDR_CONFIG_DIR           "/etc/fdr.d"
#define FDR_TRACE_INST_DIR       "/sys/kernel/tracing/instances"
#define FDR_DEBUG_INST_DIR       "/sys/kernel/debug/tracing/instances"
#define FDR_MAX_CHILDREN         64
#define FDR_HTTP_PORT_DEFAULT    9119
#define FDR_HTTP_ADDR_DEFAULT    "127.0.0.1"

#define FDR_MINFREE_DEFAULT      5

#if defined(__GNUC__) || defined(__clang__)
#define FDR_PRINTF(fmt_index, arg_index) \
	__attribute__((format(printf, fmt_index, arg_index)))
#else
#define FDR_PRINTF(fmt_index, arg_index)
#endif

#define FDR_EC_MKDIR             1
#define FDR_EC_SYSTEM            2
#define FDR_EC_SYNTAX            3
#define FDR_EC_OPEN              5
#define FDR_EC_WRITE             6
#define FDR_EC_OPENLOG           7
#define FDR_EC_OPENTRACE         8
#define FDR_EC_FSTAT             9
#define FDR_EC_MALLOC            10
#define FDR_EC_FORK              12
#define FDR_EC_BADARGS           14
#define FDR_EC_EXEC              15
#define FDR_EC_CONFIG            16
#define FDR_EC_HTTP              17

enum fdr_item_type {
	FDR_ITEM_INSTANCE = 0,
	FDR_ITEM_MODPROBE,
	FDR_ITEM_ENABLE,
	FDR_ITEM_DISABLE,
	FDR_ITEM_SAVETO,
	FDR_ITEM_MINFREE,
};

struct fdr_item {
	struct fdr_item *next;
	enum fdr_item_type type;
	char verb[32];
	char target[FDR_PATH_MAX];
	char fpath[FDR_PATH_MAX];
	char optarg[FDR_CONFIG_LINE_MAX];
	int line;
};

struct fdr_instance {
	struct fdr_instance *next;
	struct fdr_item *items;
	char iname[FDR_NAME_MAX];
	char dname[FDR_PATH_MAX];
	uint64_t bufsize_kb;
	uint64_t maxsize;       /* UINT64_MAX means unlimited. */
	uint64_t last_trace_overruns;
	uint64_t last_trace_dropped;
	uint64_t last_trace_commit_overruns;
	int minfree;
	int has_saveto;
	int trace_loss_reported;
};

/* Shared across the parent and workers through MAP_SHARED. */
struct fdr_metrics {
	uint64_t bytes_written;
	uint64_t bytes_dropped;
	uint64_t rotations;
	uint64_t probe_failures;
	uint64_t write_errors;
	uint64_t reloads;
	uint64_t trace_overruns;
	uint64_t trace_dropped_events;
	uint64_t trace_commit_overruns;
	int instances;
	int workers_alive;
	int healthy;
};

struct fdr_runtime {
	int verbose;
	int json_log;
	volatile sig_atomic_t got_sighup;
	volatile sig_atomic_t got_sigchld;
	volatile sig_atomic_t want_reload;
	volatile sig_atomic_t want_exit;
	int parse_only;
	int foreground;
	int http_port;
	int exit_status;
	int reloading;
	int num_children;
	pid_t child_pids[FDR_MAX_CHILDREN];
	int child_persistent[FDR_MAX_CHILDREN];
	struct fdr_instance *child_instances[FDR_MAX_CHILDREN];
	char http_addr[64];
	char inst_dir[FDR_PATH_MAX];
	char config_dir[FDR_PATH_MAX];
	struct fdr_instance *instances;
	int instance_count;
	struct fdr_metrics *metrics;
};

extern struct fdr_runtime fdr;

void fdr_die(int code, const char *fmt, ...) FDR_PRINTF(2, 3);
void fdr_log(const char *level, const char *fmt, ...) FDR_PRINTF(2, 3);
void fdr_warn(const char *fmt, ...) FDR_PRINTF(1, 2);

int fdr_copy_field(char *dst, size_t dstsz, const char *src);
int fdr_join_path(char *dst, size_t dstsz, const char *a, const char *b);
void fdr_chomp_line(char *buf);
int fdr_parse_size(const char *arg, uint64_t *value);
const char *fdr_default_inst_dir(void);
int fdr_write_all(int fd, const void *buf, size_t len);

void fdr_metrics_init(void);
void fdr_metrics_destroy(void);
void fdr_metrics_add(uint64_t *counter, uint64_t delta);
uint64_t fdr_metrics_load_u64(const uint64_t *counter);
int fdr_metrics_load_int(const int *value);
void fdr_metrics_store_int(int *value, int new_value);

void fdr_instance_init(struct fdr_instance *insp);
void fdr_instance_append(struct fdr_instance *insp);
void fdr_config_free(void);
int fdr_config_load(const char *dir);
int fdr_config_parse_line_test(struct fdr_instance *insp, const char *line);

int fdr_trace_create_instance(struct fdr_instance *insp,
    const struct fdr_item *item);
int fdr_trace_load_module(const struct fdr_item *item);
int fdr_trace_set_probe(struct fdr_instance *insp,
    const struct fdr_item *item);
int fdr_trace_sample_loss(struct fdr_instance *insp);
void fdr_trace_sample_all_loss(void);

int fdr_harvest_run(struct fdr_instance *insp, const struct fdr_item *item);

int fdr_process_start_all(void);
void fdr_process_stop_children(void);
void fdr_process_cleanup_instances(void);
void fdr_process_install_handlers(void);
void fdr_process_reap_children(void);
int fdr_process_reload(void);

int fdr_http_serve(const char *address, int port);

#endif /* FDR_H */
