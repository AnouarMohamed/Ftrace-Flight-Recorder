# Flight Data Recorder

FDR is a small Linux daemon that creates isolated ftrace instances, enables
configured kernel tracepoints, and optionally persists the event stream. It is
intended for low-level incident evidence: scheduler, network, filesystem, and
other kernel events that are useful after a failure.

FDR runs as root and the Kubernetes deployment is privileged because tracefs is
a host-kernel control interface. Treat write access to <code>/etc/fdr.d</code>
or the Kubernetes ConfigMap as equivalent to administrative access.

## Features

- One isolated worker and ftrace instance per configuration file
- Strict configuration validation without requiring root (<code>fdrd -n</code>)
- Disk free-space protection that drops trace data before filling the disk
- Size-based rotation through logrotate, with a preserved <code>.1</code> fallback
- Safe, transactional configuration reload on SIGHUP
- Worker supervision: a failed persistent collector stops the parent so the
  service manager can restart it
- HTTP liveness, readiness, and Prometheus metrics
- systemd, OCI image, Kustomize, and Helm deployment support

## Configuration

FDR reads regular <code>*.conf</code> files directly under
<code>/etc/fdr.d</code>, in lexical order. Each file defines exactly one
instance, and its first directive must be <code>instance</code>. Blank lines
and lines whose first non-space character is <code>#</code> are ignored.
Inline comments are not supported because probe filters may contain spaces.

### instance name [buffer-size]

Creates an isolated ftrace instance. Names may contain letters, digits,
<code>.</code>, <code>_</code>, and <code>-</code>.

<code>buffer-size</code> is a byte size and accepts <code>k</code>,
<code>m</code>, or <code>g</code> suffixes, with optional
<code>iB</code>/<code>B</code>. FDR converts it to the KiB-per-CPU unit
required by the kernel's <code>buffer_size_kb</code> interface. Be
conservative: the allocation is made for every CPU.

### modprobe module-name

Loads a required module without invoking a shell.

### enable subsystem/event [filter]

Applies the optional ftrace filter first, then enables the event. Use
<code>subsystem/all</code> to enable a complete subsystem.

### disable subsystem/event

Disables an event. <code>subsystem/all</code> disables the complete subsystem.

### minfree percentage

Drops incoming trace data while available space on the destination filesystem
is at or below this percentage. The value must be from 1 through 100 and
defaults to 5.

### saveto absolute-path [maximum-size]

Continuously appends trace output to a regular file created with mode 0600.
There may be one <code>saveto</code> per instance.

Without <code>maximum-size</code>, the file is unlimited except for
<code>minfree</code>. At the limit, FDR runs
<code>/usr/sbin/logrotate -f /etc/logrotate.d/&lt;instance&gt;</code> when
that configuration exists. Otherwise it preserves the previous file as
<code>&lt;absolute-path&gt;.1</code> and starts a new file. Existing logs are
never truncated at startup.

Without <code>saveto</code>, FDR configures the instance and the setup worker
exits successfully. Data can then be consumed manually from:

~~~text
/sys/kernel/tracing/instances/<name>/trace_pipe
~~~

Example:

~~~text
instance node 16m
enable sched/sched_switch
enable sched/sched_wakeup target_cpu >= 0
minfree 5
saveto /var/log/fdr/node.log 64m
~~~

## Running on a Linux host

Requirements are a C11 compiler, GNU Make, tracefs, and optionally
<code>kmod</code> and <code>logrotate</code>.

~~~sh
make check
sudo make install
sudo install -m 0644 samples/nfs /etc/fdr.d/nfs.conf
sudo systemctl enable --now fdr
~~~

The systemd service runs in the foreground, restarts on collector failure, and
binds its HTTP endpoint to <code>127.0.0.1:9119</code>.

Useful commands:

~~~sh
fdrd -n -c /etc/fdr.d
fdrd -f -j
fdrd -f -a 0.0.0.0 -p 9119
fdrd -f -p 0
fdrd -V
systemctl reload fdr
~~~

Signals:

- SIGHUP to the parent validates and atomically activates current config files.
  Invalid new configuration is rejected and existing workers stay active.
- SIGUSR1 to all <code>fdrd</code> processes tells collectors to reopen their
  log files. The sample logrotate configuration uses this signal.
- SIGTERM and SIGINT stop workers and remove trace instances.

## HTTP endpoints

- <code>/healthz</code> — parent event loop is alive
- <code>/readyz</code> — configured probes and collectors are healthy
- <code>/metrics</code> — Prometheus text exposition

The default listener is <code>127.0.0.1:9119</code>. Authentication and TLS
are deliberately out of scope; use a firewall, sidecar, or reverse proxy before
exposing it outside a trusted network.

## Kubernetes

FDR runs once per node and mounts the host tracefs and
<code>/var/log/fdr</code>.

~~~sh
kubectl apply -k deploy/kubernetes

helm upgrade --install fdr deploy/helm/fdr \
  --namespace fdr-system --create-namespace
~~~

See the [Kustomize guide](deploy/kubernetes/README.md) and
[Helm guide](deploy/helm/fdr/README.md). Both deployments include startup,
liveness, and readiness probes. Kustomize hashes generated configuration names,
and Helm annotates the pod template with a configuration checksum, so config
changes roll the DaemonSet automatically.

## Development

~~~sh
make check
make sanitize
helm lint deploy/helm/fdr
kubectl kustomize deploy/kubernetes >/dev/null
~~~

The tests use a temporary fake tracefs and never modify the host kernel. A final
release should also be exercised on a disposable Linux node against the kernel
versions it intends to support.

| File | Responsibility |
|---|---|
| <code>main.c</code> | CLI and lifecycle |
| <code>config.c</code> | Strict, reloadable configuration |
| <code>trace.c</code> | Trace instances, modules, probes, filters |
| <code>harvest.c</code> | Trace consumption, disk limits, rotation |
| <code>process.c</code> | Workers, signals, supervision, reload |
| <code>http.c</code> | Health, readiness, metrics, parent event loop |
| <code>runtime.c</code> | Logging and shared metrics |
| <code>util.c</code> | Checked strings, sizes, and I/O |

## License

Universal Permissive License 1.0; see [LICENSE.txt](LICENSE.txt).
