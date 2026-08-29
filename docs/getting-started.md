# Getting started on a Linux host

This guide builds FDR from source, starts one bounded scheduler capture under
systemd, and verifies both the saved events and evidence-integrity endpoints.

For Kubernetes, use the [Kustomize guide](../deploy/kubernetes/README.md) or the
[Helm guide](../deploy/helm/fdr/README.md) instead.

## 1. Check the host

FDR requires Linux tracefs and permission to control tracing. Check the mount:

```sh
findmnt -T /sys/kernel/tracing
test -d /sys/kernel/tracing/instances
```

The filesystem type should be `tracefs`. If it is absent, review the host's
security and boot policy. On an approved system it can normally be mounted with:

```sh
sudo mount -t tracefs tracefs /sys/kernel/tracing
```

Confirm that the events used by the first configuration exist:

```sh
test -d /sys/kernel/tracing/events/sched/sched_switch
test -d /sys/kernel/tracing/events/sched/sched_wakeup
```

Install a C11 compiler and GNU Make. Install `kmod` if any configuration will
use `modprobe`, and `logrotate` if an external rotation policy will be used.

## 2. Build and test

```sh
make check
make
./fdrd -V
```

`make check` uses a temporary fake tracefs. It does not modify the host kernel.

Optional sanitizer checks require Clang:

```sh
make sanitize
```

## 3. Install

```sh
sudo make install
sudo install -d -m 0700 /var/log/fdr
sudo install -m 0644 deploy/kubernetes/fdr.conf /etc/fdr.d/node.conf
```

The install target places:

- `fdrd` in `/usr/sbin`;
- `fdr.service` in the systemd unit directory;
- the manual page in section 8;
- samples and the README under `/usr/share/fdr`;
- an empty configuration directory at `/etc/fdr.d`.

It does not overwrite an existing configuration.

## 4. Review and validate the configuration

The first configuration records scheduler switches and wakeups:

```text
instance node
enable sched/sched_switch
enable sched/sched_wakeup
minfree 5
saveto /var/log/fdr/node.log 64m
```

Validate its syntax without accessing tracefs:

```sh
sudo fdrd -n -c /etc/fdr.d
```

This validation catches grammar and value errors. It cannot prove that a
tracepoint or filter exists on the running kernel because `-n` deliberately
does not access tracefs.

Read the [configuration reference](configuration.md) before enabling additional
events or changing buffer and file sizes.

## 5. Start FDR

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now fdr
systemctl status --no-pager fdr
```

Follow startup logs:

```sh
journalctl -u fdr -f
```

Expected messages include the loaded instance, created trace instance, started
worker, output file, and HTTP listener.

## 6. Verify the recorder

Generate ordinary scheduler activity, then check the capture:

```sh
for i in 1 2 3 4 5; do sh -c :; done
sudo test -s /var/log/fdr/node.log
sudo grep -m 1 sched_switch /var/log/fdr/node.log
```

Check the endpoints:

```sh
curl --fail http://127.0.0.1:9119/healthz
curl --fail http://127.0.0.1:9119/readyz
curl --silent http://127.0.0.1:9119/metrics
```

A good initial state has:

- `healthz` returning HTTP 200 and `ok`;
- `readyz` returning HTTP 200 and `ready`;
- `fdr_ready 1`;
- the three `fdr_trace_*` loss counters at zero;
- `fdr_workers_alive` equal to the number of persistent configurations.

## 7. Make a safe configuration change

Edit a `*.conf` file, validate the entire directory, then reload:

```sh
sudo fdrd -n -c /etc/fdr.d
sudo systemctl reload fdr
journalctl -u fdr --since '-2 minutes' --no-pager
curl --fail http://127.0.0.1:9119/readyz
```

An invalid directory is rejected and the existing workers remain active. A
valid reload replaces the current workers and trace instances, so there is a
brief capture transition. Preserve incident evidence before reloading.

## 8. Stop and remove

```sh
sudo systemctl disable --now fdr
sudo make uninstall
```

Graceful shutdown removes FDR's tracefs instances. Capture files and
`/etc/fdr.d` remain for the administrator to retain or remove deliberately.

## Next steps

- [Configuration reference](configuration.md)
- [Operations and troubleshooting](operations.md)
- [Security policy](../SECURITY.md)
- [Validation evidence](validation/README.md)
