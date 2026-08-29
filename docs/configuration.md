# Configuration reference

FDR reads configuration from a directory, `/etc/fdr.d` by default. Each regular
file whose name ends in `.conf` defines exactly one tracefs instance.

## File discovery and parsing

- Files are read in lexical path order.
- Only regular files directly under the directory are accepted.
- At least one `*.conf` file is required.
- Instance names must be unique across the directory.
- At most 64 instances can be active in one daemon.
- The first non-comment directive in every file must be `instance`.
- Blank lines and lines whose first non-space character is `#` are ignored.
- Inline comments are not supported because event filters may contain spaces.
- Directives are applied in file order. Put `modprobe` before events that depend
  on the module, and use deliberate ordering when enabling and disabling the
  same event.

Validate the complete directory without touching tracefs:

```sh
fdrd -n -c /etc/fdr.d
```

Parse-only validation does not confirm that kernel modules, events, filters,
tracefs permissions, output directories, or free-space conditions are valid at
runtime.

## Directives

| Directive | Required | Purpose |
|---|---|---|
| `instance name [buffer-size]` | First line | Creates the isolated tracefs instance |
| `modprobe module-name` | No | Loads a required kernel module without a shell |
| `enable subsystem/event [filter]` | No | Applies an optional filter and enables an event |
| `disable subsystem/event` | No | Disables one event or a complete subsystem |
| `minfree percentage` | No | Drops output data when destination free space is too low |
| `saveto absolute-path [maximum-size]` | No | Continuously persists `trace_pipe` to a file |

### `instance name [buffer-size]`

Creates `/sys/kernel/tracing/instances/<name>` or the corresponding legacy
debugfs path.

Names can contain letters, digits, `.`, `_`, and `-`. The names `.` and `..`
are rejected.

If a buffer size is supplied, FDR converts the byte value to the kernel's
`buffer_size_kb` unit. That size is allocated per CPU, not per host. For
example, `instance node 16m` requests roughly 16 MiB on every CPU. The total
kernel memory commitment therefore grows with CPU count, and the kernel may
round the requested value.

Without a size, the kernel's instance default is retained.

### Size syntax

Buffer and file sizes are integer byte values. Suffixes are binary multiples:

| Example | Value |
|---|---:|
| `4096` | 4,096 bytes |
| `64k`, `64KB`, `64KiB` | 65,536 bytes |
| `16m`, `16MB`, `16MiB` | 16 MiB |
| `1g`, `1GB`, `1GiB` | 1 GiB |

Values must be positive where a size is accepted. Fractions such as `1.5m` are
not supported.

### `modprobe module-name`

Runs `modprobe -- <module-name>` directly, without shell interpolation. Module
names can use letters, digits, `.`, `_`, `-`, and `:`.

Module loading changes the host kernel. Use it only when a reviewed event set
requires a module. Kubernetes deployments must also expose the matching host
module tree; this is disabled by default.

### `enable subsystem/event [filter]`

Enables a kernel tracepoint such as:

```text
enable sched/sched_switch
```

For a specific event, everything after the event name is written to the
event's ftrace `filter` file before the event is enabled:

```text
enable sched/sched_wakeup target_cpu >= 0
```

Filter syntax and available fields belong to the running kernel. Inspect them
before deployment:

```sh
cat /sys/kernel/tracing/events/sched/sched_wakeup/format
cat /sys/kernel/tracing/events/sched/sched_wakeup/filter
```

Use `subsystem/all` to enable every event in a subsystem:

```text
enable sched/all
```

Whole-subsystem capture can have very high event volume. Do not treat it as a
safe production default, and do not use controlled overloads on a shared host.

If an event or filter cannot be applied, FDR records a probe failure and starts
the instance in a degraded readiness state. Other valid probes continue.

### `disable subsystem/event`

Disables a specific event:

```text
disable sched/sched_wakeup
```

Use `subsystem/all` to disable all events in one subsystem. Ordering matters if
the same event or subsystem is enabled elsewhere in the file.

### `minfree percentage`

Sets the minimum available-space threshold for the filesystem containing the
`saveto` file. The value must be from 1 through 100 and defaults to 5.

When available space is at or below the threshold, the collector continues
reading the kernel trace pipe but discards those bytes instead of filling the
filesystem. Discarded bytes increase `fdr_bytes_dropped_total`. The check is
periodic, so this is a protection mechanism rather than an exact disk quota.

### `saveto absolute-path [maximum-size]`

Starts a persistent collector that appends events to an absolute path:

```text
saveto /var/log/fdr/node.log 64m
```

Only one `saveto` directive is allowed per instance. The parent directory must
already exist. New files are created with mode 0600, final symlinks are not
followed when supported by the operating system, and the target must be a
regular file. Existing files are appended and are never truncated at startup.

Without a maximum size, the file is unbounded except for `minfree` protection.
With a maximum size, FDR rotates before writing a block that would cross the
limit:

1. If `/etc/logrotate.d/<instance>` is a regular file, FDR runs
   `/usr/sbin/logrotate -f` with that policy.
2. Otherwise FDR renames the current file to `<path>.1` and opens a new file.

The fallback retains one previous generation. A failed rotation drops the
current trace block, increments `fdr_bytes_dropped_total` and
`fdr_rotation_failures_total`, and makes readiness false. Further blocks are
accounted as dropped, but FDR paces expensive rotation retries to once per
second. If the underlying permission or storage problem recovers, a later retry
rotates successfully and collection resumes; readiness remains latched false
because evidence was already lost.

### Setup-only instances

If `saveto` is omitted, FDR configures the trace instance and the worker exits
successfully. An external consumer can read:

```text
/sys/kernel/tracing/instances/<name>/trace_pipe
```

The parent keeps the trace instance until shutdown or reload. The
`fdr_workers_alive` gauge counts live workers, so it will be lower than
`fdr_instances` for intentional setup-only configurations. The bundled
`FDRWorkerMissing` Prometheus alert assumes persistent `saveto` collectors.

## Examples

### Bounded scheduler history

```text
instance scheduler 16m
enable sched/sched_switch
enable sched/sched_wakeup
minfree 10
saveto /var/log/fdr/scheduler.log 128m
```

### Filtered wakeups

```text
instance wakeups 8m
enable sched/sched_wakeup target_cpu == 0
minfree 5
saveto /var/log/fdr/wakeups.log 32m
```

### Module-dependent tracepoints

```text
instance nfs 16m
modprobe nfsv4
enable nfs4/nfs4_open_expired
enable sunrpc/rpc_socket_error
minfree 10
saveto /var/log/fdr/nfs.log 128m
```

Event names differ across kernel versions. Treat the supplied
[`samples/nfs`](../samples/nfs) file as an example, not a portable default.

## Choosing safe values

### Events

Start with a small set tied to a concrete incident question. Measure event rate
under representative load before adding broad subsystems.

### Buffer size

A larger ring buffer retains a longer burst but consumes the requested memory
on every CPU. Monitor the trace-loss counters; zero loss under one workload does
not guarantee zero loss under another.

### File size and retention

Choose the file limit from event rate, incident response time, and available
storage. The built-in fallback keeps only the current file and `.1`; use a
reviewed logrotate policy for more generations.

### Free-space threshold

Set `minfree` high enough to protect the host's other workloads. Storage drops
mean the resulting capture is incomplete even though the daemon may remain
ready, so alert on increases in `fdr_bytes_dropped_total`.

## Activating changes

On a systemd host:

```sh
sudo fdrd -n -c /etc/fdr.d
sudo systemctl reload fdr
```

An invalid new directory is rejected without stopping active workers. A valid
reload stops the old workers, removes their instances, and starts the new set.
See [Reload behavior](operations.md#reload-behavior) before using reload during
an incident.
