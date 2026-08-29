# Operations guide

This guide covers day-to-day operation, monitoring, incident preservation,
reloads, rotation, and troubleshooting. Read the
[configuration reference](configuration.md) first when choosing events or
sizing buffers and files.

## Command-line reference

```text
fdrd [-fjnvV] [-a address] [-p port] [-c config-directory] [-d tracefs-root]
```

| Option | Meaning | Default |
|---|---|---|
| `-a address` | IPv4 address for health and metrics | `127.0.0.1` |
| `-p port` | HTTP port; `0` disables HTTP | `9119` |
| `-c directory` | Directory containing `*.conf` files | `/etc/fdr.d` |
| `-d directory` | Tracefs root; FDR appends `instances` | Auto-detected |
| `-f` | Stay in the foreground | Daemonize |
| `-j` | Emit one JSON object per log line | Plain-text logs |
| `-n` | Parse and validate configuration, then exit | Run normally |
| `-v` | Increase logging verbosity | Normal verbosity |
| `-V` | Print the version and exit | — |

Without `-d`, FDR prefers `/sys/kernel/tracing` and falls back to
`/sys/kernel/debug/tracing`. Systemd and container deployments use `-f` so the
service manager can supervise the parent process.

## Routine checks

On a systemd host:

```sh
systemctl is-active fdr
systemctl status --no-pager fdr
journalctl -u fdr --since '15 minutes ago' --no-pager
curl --fail http://127.0.0.1:9119/healthz
curl --fail http://127.0.0.1:9119/readyz
curl --silent http://127.0.0.1:9119/metrics
```

On Kubernetes:

```sh
kubectl -n fdr-system get pods -l app=fdr -o wide
kubectl -n fdr-system logs -l app=fdr -c fdrd --tail=200
kubectl -n fdr-system port-forward daemonset/fdr 9119:9119
curl --fail http://127.0.0.1:9119/readyz
```

Helm release labels differ from the plain manifests. Use
`kubectl -n NAMESPACE get pods` to find the pod if `app=fdr` selects nothing.

For every persistent instance, confirm all of the following:

- the process or pod is live;
- `/readyz` returns HTTP 200 and `ready`;
- `fdr_ready` is `1`;
- trace-loss, write-error, and storage-drop counters are not increasing;
- the capture file is a non-empty regular file owned according to local policy;
- `fdr_workers_alive` matches `fdr_instances` unless setup-only instances are
  intentional.

## Logs

FDR writes logs to standard error. The systemd unit sends them to the journal,
and Kubernetes captures them as container logs. Plain lines contain an RFC 3339
timestamp, severity, and message:

```text
2026-08-29T12:00:00Z [info] saving instance node to /var/log/fdr/node.log
```

Pass `-j` for one JSON object per line. Each object contains `ts`, `level`,
`msg`, and `pid`. Pass `-v` to include successful per-probe changes and
setup-only worker completion. Do not depend on human message text as a stable
machine API; use metrics for alerts and structured logs for transport.

## HTTP endpoints

The listener supports `GET` and has no authentication or TLS.

| Endpoint | Success | Meaning |
|---|---|---|
| `/healthz` | `200 ok` | The parent event loop is alive |
| `/readyz` | `200 ready` | Current recorder state has no known integrity failure |
| `/readyz` | `503 not ready` | A probe, collector, storage, write, or trace-loss failure was observed |
| `/metrics` | `200` | Current Prometheus-format metrics |
| Any other path | `404` | Unknown endpoint |

Liveness is not evidence completeness. A healthy process can be unready. Any
known storage drop now latches readiness false because the persisted evidence
is incomplete. Monitor all integrity signals; readiness cannot prove that an
unobserved loss did not occur.

## Metric reference

FDR samples the kernel's trace-loss statistics every five seconds. All
counters are cumulative for the current parent-process lifetime.

| Metric | Type | Interpretation and response |
|---|---|---|
| `fdr_bytes_written_total` | Counter | Bytes successfully appended. Useful for volume, not proof of completeness. |
| `fdr_bytes_dropped_total` | Counter | Bytes discarded by free-space protection or a failed rotation. Any increase means incomplete file evidence; investigate disk capacity and rotation. |
| `fdr_rotations_total` | Counter | Successful bounded-file rotations. Confirm retention behaves as intended. |
| `fdr_rotation_failures_total` | Counter | Rotation attempts that failed before a retry. An increase makes readiness false; inspect permissions, free space, and logrotate. |
| `fdr_probe_failures_total` | Counter | Events, filters, or modules that could not be configured. An increase makes readiness false. |
| `fdr_write_errors_total` | Counter | Capture write or collector errors. Preserve logs and storage state immediately. |
| `fdr_reloads_total` | Counter | Accepted `SIGHUP` configuration reloads. This does not count rejected syntax. |
| `fdr_trace_overruns_total` | Counter | Kernel ring-buffer overruns observed across instances and CPUs. Lost events cannot be recovered. |
| `fdr_trace_dropped_events_total` | Counter | Events the kernel reports as dropped. Lost events cannot be recovered. |
| `fdr_trace_commit_overruns_total` | Counter | Kernel commit overruns observed in tracefs statistics. Treat as incomplete evidence. |
| `fdr_instances` | Gauge | Number of configured tracefs instances owned by the parent. |
| `fdr_workers_alive` | Gauge | Workers currently alive. Persistent `saveto` instances normally have one worker each. |
| `fdr_ready` | Gauge | `1` when ready, `0` after a known integrity or setup failure. |

Useful Prometheus expressions:

```promql
fdr_ready == 0
fdr_workers_alive < fdr_instances
increase(fdr_bytes_dropped_total[5m]) > 0
increase(fdr_rotation_failures_total[5m]) > 0
increase(fdr_write_errors_total[5m]) > 0
increase(fdr_probe_failures_total[5m]) > 0
increase(fdr_trace_overruns_total[5m])
  + increase(fdr_trace_dropped_events_total[5m])
  + increase(fdr_trace_commit_overruns_total[5m]) > 0
```

The Helm chart can install a `PodMonitor`, six alert rules, and a Grafana
dashboard for these signals. See the [Helm observability
guide](../deploy/helm/fdr/README.md#prometheus-alerts-grafana-and-network-policy).

### Reading readiness correctly

| State | Meaning | Action |
|---|---|---|
| Healthy and ready, counters stable | No known current failure | Continue monitoring; this is not a guarantee against unobserved loss |
| Healthy but unready | Parent lives, but evidence is known to be degraded | Preserve evidence and metrics, then diagnose the changing counter or logs |
| Unhealthy or process absent | Recorder is not being supervised successfully | Inspect the service manager, exit status, and previous logs |
| Unready with storage drops increasing | Free-space protection or rotation is discarding output | Treat the capture as incomplete and repair storage before reloading |

Readiness is intentionally sticky after a detected failure. A successful reload
or restart creates a fresh health state, but it cannot repair the already
incomplete capture.

## Incident evidence workflow

Preserve volatile evidence before reloading, restarting, or changing probes:

1. Record the UTC time, hostname or node, kernel version, FDR version, source
   revision or image digest, and active configuration.
2. Save `/metrics` while its cumulative counters still describe the failure.
3. Copy the current capture and any `.1` or logrotate generations without
   editing the originals.
4. Save service logs, pod logs, Kubernetes events, and the previous container
   log when a restart occurred.
5. If access is safe, preserve each instance's `per_cpu/*/stats` files.
6. Only after collection, reload, restart, or change the workload.

Example host collection:

```sh
date --utc --iso-8601=seconds
uname -a
fdrd -V
curl --silent http://127.0.0.1:9119/metrics > fdr-metrics.txt
sudo cp -a /etc/fdr.d ./fdr-config
sudo cp -a /var/log/fdr ./fdr-captures
journalctl -u fdr --since '2 hours ago' --no-pager > fdr-journal.txt
```

Store collected evidence in a protected directory: kernel traces can contain
process names, paths, identifiers, and workload timing. Do not publish captures
or credentials in an issue report.

## Reload behavior

Validate before requesting a reload:

```sh
sudo fdrd -n -c /etc/fdr.d
sudo systemctl reload fdr
```

On `SIGHUP`, the parent:

1. parses the complete new configuration into memory;
2. rejects invalid configuration while leaving current workers active;
3. for valid configuration, stops workers and removes their trace instances;
4. creates the new instances and starts new workers;
5. increments `fdr_reloads_total` and resets the readiness state for the new
   configuration.

Reload is safe from partial syntax activation, but it is not capture-gap-free.
Runtime failures can still occur after parsing because `-n` cannot verify
kernel event availability, filter validity, module availability, filesystem
permissions, or free space. Watch readiness and logs immediately afterward.

## Worker supervision

Each configuration file receives one worker. A `saveto` worker is expected to
remain alive and drain its trace pipe. If a persistent collector exits
unexpectedly, the parent exits so systemd or Kubernetes can restart the whole
recorder and reconstruct a consistent set of instances.

A configuration without `saveto` is setup-only: its worker exits successfully
after enabling probes, while its tracefs instance remains available to an
external reader. In that model, `fdr_workers_alive < fdr_instances` is expected
and the bundled `FDRWorkerMissing` alert must be adjusted or disabled.

## File rotation

When `saveto` has a maximum size, FDR rotates internally before a write would
cross the limit. It uses `/etc/logrotate.d/<instance>` when that file is
regular; otherwise it preserves one fallback generation as `<capture>.1`.
See [the exact rotation behavior](configuration.md#saveto-absolute-path-maximum-size).

For an external rotation process, move the current file first and then tell all
FDR processes to reopen it:

```sh
sudo systemctl kill --kill-who=all --signal=SIGUSR1 fdr
```

Do not signal only the parent: collectors receive `SIGUSR1` and reopen their
own output files. Confirm that the new file exists, is regular, has the intended
ownership, and is receiving data.

## Signals

| Signal | Behavior |
|---|---|
| `SIGHUP` | Validate and replace the active configuration |
| `SIGUSR1` | Make collector workers reopen output files |
| `SIGTERM`, `SIGINT` | Stop workers, remove owned trace instances, and exit |

Send termination through systemd, Kubernetes, or the owning supervisor so it
can enforce timeouts and record the exit.

## Troubleshooting

| Symptom | Likely cause | Checks and response |
|---|---|---|
| `no configuration files` | No regular `*.conf` exists directly in the selected directory | Check `-c`, filenames, permissions, and `fdrd -n` output |
| Tracefs or `instances` missing | Tracefs is not mounted or the wrong root was selected | Run `findmnt -T /sys/kernel/tracing`; inspect `-d`; review host policy before mounting |
| Kubernetes preflight fails | HostPath is not writable tracefs or lacks `instances` | Read the `tracefs-preflight` init-container log and verify the node mount |
| `/readyz` returns 503 | Probe, collector, write, or trace-loss failure | Compare counters, inspect logs, and preserve evidence before resetting state |
| Probe failures increase | Event is absent, filter is invalid, module failed, or access was denied | Inspect the running kernel's event `format` and `filter`; validate on every supported kernel |
| Trace-loss counters increase | Event rate exceeded buffer or collector capacity | Preserve evidence; narrow probes, enlarge per-CPU buffers, remove CPU throttling, or improve storage after representative testing |
| Storage drops increase | `minfree` threshold was reached or rotation failed | Check filesystem capacity, inode availability, output path, and logrotate policy |
| Write errors increase | Filesystem, permission, file-type, or I/O failure | Inspect logs and mount state; verify parent directory and regular-file target |
| Worker count is low | Collector exited, or an instance intentionally omits `saveto` | Compare configuration to process logs; treat persistent collector loss as a fault |
| Container keeps restarting | Persistent collector failure or startup/preflight failure | Inspect current and `--previous` logs, pod events, and termination status |
| HTTP connection refused | HTTP disabled, wrong bind address, daemon absent, or no port-forward | Check `-a`, `-p`, process state, and networking; `-p 0` disables HTTP |
| Trace instance remains after a crash | Process could not perform graceful cleanup | Stop all FDR processes, preserve needed evidence, then remove only the confirmed FDR-owned instance |

Never remove an unfamiliar tracefs instance. Another tracing tool may own it.

## Security and production limits

- Restrict configuration, executable, image, and service-definition changes to
  trusted administrators.
- Keep the HTTP endpoint on loopback or a protected monitoring network. It has
  no application authentication.
- Treat a privileged FDR container as a host-root security boundary.
- Measure selected events on representative kernels and workloads. Broad
  subsystems and CPU throttling can cause trace loss.
- Validate every kernel version separately because event names and fields can
  change.
- Use disposable VMs for deliberate overload and loss testing.

See [SECURITY.md](../SECURITY.md), the [validation evidence
guide](validation/README.md), and the [roadmap](../ROADMAP.md) for known limits
and remaining release gates.
