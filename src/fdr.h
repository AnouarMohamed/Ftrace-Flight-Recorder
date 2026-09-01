/*
 * fdr.h - Flight Data Recorder shared definitions and architecture interfaces
 *
 * Licensed under the Universal Permissive License (UPL), Version 1.0.
 *
 * Architecture Overview:
 * FDR (Flight Data Recorder) continuously captures Linux kernel ftrace events
 * into bounded log files with strict evidence-integrity guarantees.
 *
 * Multi-Process Model:
 * - Parent Supervisor: Parses configuration files in /etc/fdr.d, allocates
 *   tracefs instances (/sys/kernel/tracing/instances/<name>), spawns worker
 *   processes, reaps dead workers, monitors kernel trace-loss statistics,
 *   handles transactional SIGHUP reloads, and serves HTTP /healthz, /readyz,
 *   and /metrics endpoints.
 * - Worker Processes: Each instance configuration runs in an isolated child
 *   worker. The worker configures probes, loads required modules, and drains
 *   the kernel trace_pipe into a mode-0600 log file while enforcing minfree
 *   filesystem thresholds and bounded log rotation.
 * - Shared State: Shared metrics across processes are mapped via anonymous
 *   shared memory (mmap MAP_SHARED | MAP_ANONYMOUS) with relaxed atomic
 *   counter updates.
 */

#ifndef FDR_H
#define FDR_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Buffer & Path Capacity Limits
 */

/** Maximum length for tracefs instance and configuration names. */
#define FDR_NAME_MAX             256

/** Maximum filesystem path length for configuration, logs, and tracefs files. */
#define FDR_PATH_MAX             4096

/** Maximum line length allowed in an FDR configuration file. */
#define FDR_CONFIG_LINE_MAX      4096

/** Default configuration directory scanned for *.conf files. */
#define FDR_CONFIG_DIR           "/etc/fdr.d"

/** Primary Linux tracefs instance root mount path. */
#define FDR_TRACE_INST_DIR       "/sys/kernel/tracing/instances"

/** Legacy debugfs tracing instance mount path (fallback). */
#define FDR_DEBUG_INST_DIR       "/sys/kernel/debug/tracing/instances"

/** Maximum number of concurrent instance workers supervised by one daemon. */
#define FDR_MAX_CHILDREN         64

/** Default port for the HTTP health, readiness, and Prometheus listener. */
#define FDR_HTTP_PORT_DEFAULT    9119

/** Default IPv4 bind address (loopback) for the HTTP listener. */
#define FDR_HTTP_ADDR_DEFAULT    "127.0.0.1"

/** Default percentage of free filesystem space required before dropping data. */
#define FDR_MINFREE_DEFAULT      5

/*
 * Compiler Attributes & Printf Format Checking
 */
#if defined(__GNUC__) || defined(__clang__)
#define FDR_PRINTF(fmt_index, arg_index) \
	__attribute__((format(printf, fmt_index, arg_index)))
#else
#define FDR_PRINTF(fmt_index, arg_index)
#endif

/*
 * Exit Codes (EC) & Process Error States
 * These codes represent distinct failure categories across supervisor and worker exits.
 */
#define FDR_EC_MKDIR             1   /**< Failed to create tracefs instance directory. */
#define FDR_EC_SYSTEM            2   /**< System call failure (signal, prctl, etc.). */
#define FDR_EC_SYNTAX            3   /**< Configuration parse or syntax error. */
#define FDR_EC_OPEN              5   /**< File open failure for tracefs control nodes. */
#define FDR_EC_WRITE             6   /**< Trace control or output write failure. */
#define FDR_EC_OPENLOG           7   /**< Failed to open destination capture file. */
#define FDR_EC_OPENTRACE         8   /**< Failed to open kernel trace_pipe. */
#define FDR_EC_FSTAT             9   /**< Failed to stat output log or filesystem. */
#define FDR_EC_MALLOC            10  /**< Memory allocation or mmap failure. */
#define FDR_EC_FORK              12  /**< Failed to fork a child process or worker. */
#define FDR_EC_BADARGS           14  /**< Invalid CLI command-line arguments. */
#define FDR_EC_EXEC              15  /**< Failed to execute modprobe or logrotate. */
#define FDR_EC_CONFIG            16  /**< Configuration validation or loading error. */
#define FDR_EC_HTTP              17  /**< HTTP socket binding or event loop error. */

/**
 * enum fdr_item_type - Directives supported in an FDR configuration file (*.conf).
 */
enum fdr_item_type {
	FDR_ITEM_INSTANCE = 0, /**< 'instance <name> [bufsize]': Defines the tracefs instance. */
	FDR_ITEM_MODPROBE,     /**< 'modprobe <module>': Loads a kernel module via modprobe. */
	FDR_ITEM_ENABLE,       /**< 'enable <subsystem/event> [filter]': Enables a tracepoint. */
	FDR_ITEM_DISABLE,      /**< 'disable <subsystem/event>': Disables a tracepoint. */
	FDR_ITEM_SAVETO,       /**< 'saveto <path> [maxsize]': Drains trace_pipe to disk. */
	FDR_ITEM_MINFREE,      /**< 'minfree <percentage>': Minimum free disk space threshold. */
};

/**
 * struct fdr_item - Parsed configuration directive AST node.
 * Linked list representing directives in lexical file order within an instance.
 */
struct fdr_item {
	struct fdr_item *next;               /**< Pointer to next directive in the instance list. */
	enum fdr_item_type type;             /**< Parsed directive type. */
	char verb[32];                       /**< Directive verb string (e.g., "enable", "saveto"). */
	char target[FDR_PATH_MAX];           /**< Primary target (event name, file path, module). */
	char fpath[FDR_PATH_MAX];            /**< Configuration file path where directive was parsed. */
	char optarg[FDR_CONFIG_LINE_MAX];    /**< Optional argument (event filter expression). */
	int line;                            /**< 1-indexed line number in the configuration file. */
};

/**
 * struct fdr_instance - Runtime state and configuration for one tracefs instance.
 */
struct fdr_instance {
	struct fdr_instance *next;           /**< Pointer to next instance in daemon instance list. */
	struct fdr_item *items;              /**< Head of the directive AST list for this instance. */
	char iname[FDR_NAME_MAX];            /**< Instance name (e.g. "node", "scheduler"). */
	char dname[FDR_PATH_MAX];            /**< Full tracefs directory path (/sys/kernel/tracing/instances/<iname>). */
	uint64_t bufsize_kb;                 /**< Per-CPU buffer size in KiB (0 = kernel default). */
	uint64_t maxsize;                    /**< Maximum log file size before rotation (UINT64_MAX = unlimited). */
	uint64_t last_trace_overruns;        /**< Previous sampled ring-buffer overrun counter. */
	uint64_t last_trace_dropped;         /**< Previous sampled tracefs dropped-events counter. */
	uint64_t last_trace_commit_overruns; /**< Previous sampled commit overrun counter. */
	char **trace_stats_paths;             /**< Cached per-CPU tracefs stats paths. */
	size_t trace_stats_path_count;        /**< Number of cached per-CPU stats paths. */
	unsigned int trace_stats_samples;     /**< Samples since the last topology refresh. */
	long trace_stats_online_cpus;         /**< Online CPU count at topology discovery. */
	int minfree;                         /**< Minimum percentage of free disk space required. */
	int has_saveto;                      /**< Flag: 1 if this instance drains to a file, 0 if setup-only. */
	int trace_loss_reported;             /**< Flag: 1 if loss warning has already been logged. */
};

/**
 * struct fdr_metrics - Shared operational counters and gauges mapped via MAP_SHARED.
 * All counters are cumulative for the lifetime of the parent daemon process.
 * Atomic operations (__atomic_*) are used for synchronization between workers and parent.
 */
struct fdr_metrics {
	uint64_t bytes_written;              /**< Cumulative bytes successfully written to logs. */
	uint64_t bytes_dropped;              /**< Cumulative bytes dropped due to minfree/rotation. */
	uint64_t rotations;                  /**< Cumulative successful log file rotations. */
	uint64_t rotation_failures;         /**< Cumulative log rotation execution failures. */
	uint64_t probe_failures;             /**< Cumulative tracepoint/filter configuration failures. */
	uint64_t write_errors;               /**< Cumulative write(2) or filesystem errors. */
	uint64_t reloads;                    /**< Cumulative successful SIGHUP configuration reloads. */
	uint64_t trace_overruns;             /**< Cumulative kernel ring-buffer overwrite losses. */
	uint64_t trace_dropped_events;       /**< Cumulative kernel tracefs dropped event losses. */
	uint64_t trace_commit_overruns;      /**< Cumulative kernel nested write commit overruns. */
	int instances;                       /**< Current gauge of configured tracefs instances. */
	int workers_alive;                   /**< Current gauge of running child worker processes. */
	int healthy;                         /**< Current readiness gauge (1 = ready, 0 = degraded/loss). */
};

/**
 * struct fdr_runtime - Global runtime daemon state and process supervision table.
 */
struct fdr_runtime {
	int verbose;                         /**< Verbosity level (-v increments). */
	int json_log;                        /**< Flag: 1 to emit structured JSON logs, 0 for plain text. */
	volatile sig_atomic_t got_sighup;    /**< Async signal flag: set when SIGHUP received. */
	volatile sig_atomic_t got_sigchld;   /**< Async signal flag: set when SIGCHLD received. */
	volatile sig_atomic_t want_reload;   /**< Flag: indicates pending configuration reload. */
	volatile sig_atomic_t want_exit;     /**< Flag: indicates daemon termination requested. */
	int parse_only;                      /**< Flag: -n dry-run parse and validate mode. */
	int foreground;                      /**< Flag: -f foreground execution (no daemon(3) fork). */
	int http_port;                       /**< HTTP port to listen on (0 disables HTTP server). */
	int exit_status;                     /**< Exit code to return upon daemon termination. */
	int reloading;                       /**< Flag: 1 while reloading configuration to suppress death alerts. */
	int num_children;                    /**< Number of active child workers in child_pids table. */
	pid_t child_pids[FDR_MAX_CHILDREN];  /**< Table of active child worker process IDs. */
	int child_persistent[FDR_MAX_CHILDREN]; /**< Flag per child: 1 if saveto worker, 0 if setup-only. */
	struct fdr_instance *child_instances[FDR_MAX_CHILDREN]; /**< Instance pointer associated with each child slot. */
	char http_addr[64];                  /**< HTTP bind IP address string (e.g. "127.0.0.1" or "0.0.0.0"). */
	char inst_dir[FDR_PATH_MAX];         /**< Tracefs instance root path (/sys/kernel/tracing/instances). */
	char config_dir[FDR_PATH_MAX];       /**< Directory path scanned for configuration files. */
	struct fdr_instance *instances;      /**< Head of the linked list of loaded instances. */
	int instance_count;                  /**< Total number of active loaded instances. */
	struct fdr_metrics *metrics;         /**< Pointer to mmap'd shared metrics structure. */
};

/** Global runtime daemon state singleton. */
extern struct fdr_runtime fdr;

/*
 * Logging & Diagnostics Interfaces (runtime.c)
 */

/**
 * @brief Logs an error message and terminates the process immediately.
 * @param code Process exit code (one of FDR_EC_*).
 * @param fmt Printf-style format string.
 */
void fdr_die(int code, const char *fmt, ...) FDR_PRINTF(2, 3);

/**
 * @brief Emits a structured or plain text log message with timestamp and level.
 * @param level Log severity string ("info", "warn", "error").
 * @param fmt Printf-style format string.
 */
void fdr_log(const char *level, const char *fmt, ...) FDR_PRINTF(2, 3);

/**
 * @brief Emits a warning log message without terminating.
 * @param fmt Printf-style format string.
 */
void fdr_warn(const char *fmt, ...) FDR_PRINTF(1, 2);

/*
 * Utility & String Helpers (util.c)
 */

/**
 * @brief Safely copies a string into a bounded destination buffer.
 * @return 0 on success, -1 if truncated or invalid arguments.
 */
int fdr_copy_field(char *dst, size_t dstsz, const char *src);

/**
 * @brief Safely joins two path components with a '/' into a bounded buffer.
 * @return 0 on success, -1 if truncated or invalid arguments.
 */
int fdr_join_path(char *dst, size_t dstsz, const char *a, const char *b);

/**
 * @brief Strips trailing newline (\n) and carriage return (\r) characters in-place.
 * @param buf Null-terminated string buffer.
 */
void fdr_chomp_line(char *buf);

/**
 * @brief Parses human-readable size strings (e.g. "16m", "64k", "1g", "4096") into bytes.
 * @param arg Input size string.
 * @param value Pointer to store the resulting byte count.
 * @return 0 on success, -1 on invalid syntax or integer overflow.
 */
int fdr_parse_size(const char *arg, uint64_t *value);

/**
 * @brief Auto-detects the host tracefs instance root (/sys/kernel/tracing/instances).
 * @return String path to the detected tracefs instances directory.
 */
const char *fdr_default_inst_dir(void);

/**
 * @brief Writes exact byte buffer length to a file descriptor, handling partial writes and EINTR.
 * @return 0 on complete write, -1 on error.
 */
int fdr_write_all(int fd, const void *buf, size_t len);

/*
 * Shared Metrics Interfaces (runtime.c)
 */

/** @brief Allocates and initializes the MAP_SHARED anonymous metrics segment. */
void fdr_metrics_init(void);

/** @brief Unmaps the shared metrics segment during daemon shutdown. */
void fdr_metrics_destroy(void);

/** @brief Atomically adds delta to a shared 64-bit counter using relaxed memory order. */
void fdr_metrics_add(uint64_t *counter, uint64_t delta);

/** @brief Atomically loads a 64-bit metric counter using relaxed memory order. */
uint64_t fdr_metrics_load_u64(const uint64_t *counter);

/** @brief Atomically loads an integer metric gauge using relaxed memory order. */
int fdr_metrics_load_int(const int *value);

/** @brief Atomically stores an integer metric gauge using relaxed memory order. */
void fdr_metrics_store_int(int *value, int new_value);

/*
 * Configuration Management (config.c)
 */

/** @brief Initializes default instance values (minfree, maxsize). */
void fdr_instance_init(struct fdr_instance *insp);

/** @brief Appends an instance to the global runtime instance list. */
void fdr_instance_append(struct fdr_instance *insp);

/** @brief Frees all loaded instances and their directive AST lists. */
void fdr_config_free(void);

/** @brief Scans a directory and parses all *.conf files in lexical order. */
int fdr_config_load(const char *dir);

/** @brief Test harness helper to parse a single configuration line. */
int fdr_config_parse_line_test(struct fdr_instance *insp, const char *line);

/*
 * Tracefs & Kernel Control (trace.c)
 */

/** @brief Creates the tracefs instance directory and configures per-CPU buffer size. */
int fdr_trace_create_instance(struct fdr_instance *insp, const struct fdr_item *item);

/** @brief Loads a required kernel module using modprobe without invoking a shell. */
int fdr_trace_load_module(const struct fdr_item *item);

/** @brief Enables/disables a tracepoint and configures its ftrace filter expression. */
int fdr_trace_set_probe(struct fdr_instance *insp, const struct fdr_item *item);

/** @brief Reads per-CPU loss counters (overruns/drops) and updates readiness state. */
int fdr_trace_sample_loss(struct fdr_instance *insp);

/** @brief Releases cached per-CPU loss-sampling topology for an instance. */
void fdr_trace_reset_loss_cache(struct fdr_instance *insp);

/** @brief Samples kernel trace loss across all configured active instances. */
void fdr_trace_sample_all_loss(void);

/*
 * Harvesting & Stream Persistence (harvest.c)
 */

/** @brief Main data-plane worker loop: continuously drains trace_pipe to disk. */
int fdr_harvest_run(struct fdr_instance *insp, const struct fdr_item *item);

/*
 * Process Supervision & Signals (process.c)
 */

/** @brief Spawns worker child processes for all loaded configuration instances. */
int fdr_process_start_all(void);

/** @brief Sends SIGTERM to all child workers and waits for clean exit. */
void fdr_process_stop_children(void);

/** @brief Removes tracefs instance directories created by this daemon. */
void fdr_process_cleanup_instances(void);

/** @brief Installs supervisor signal handlers (SIGTERM, SIGINT, SIGHUP, SIGCHLD). */
void fdr_process_install_handlers(void);

/** @brief Asynchronously reaps exited child processes and handles unexpected crashes. */
void fdr_process_reap_children(void);

/** @brief Performs transactional configuration reload on SIGHUP. */
int fdr_process_reload(void);

/*
 * HTTP Server & Prometheus Exposition (http.c)
 */

/** @brief Runs the HTTP server and parent event loop multiplexer. */
int fdr_http_serve(const char *address, int port);

#endif /* FDR_H */
