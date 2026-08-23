# Flight Data Recorder

## Description

The flight data recorder (fdr) is a daemon which enables ftrace probes,
harvests ftrace data and (optionally) writes the data to a file.

The behavior of fdr is defined by configuration files stored in
```/etc/fdr.d```.  During service startup, fdr will process each file in
the directory which has the suffix of .conf.  If new config files
are added to the fdr.d directory, then the service must be restarted
to recognize the new configuration information.

fdr is controlled by ```systemd(8)``` on systems where systemd is
available.  Error messages from fdr can be viewed via systemctl,
for example, ```systemctl status -l fdr```.

## Configuration File Syntax

The following keywords and options are recognized

### instance iname [buffer-size]

Create a new ftrace instance called "iname".  This instance
will appear under the kernel tracing filesystem, typically
`/sys/kernel/tracing/instances` (or the debugfs path on older kernels).

The optional buffer-size parameter can be used to control
the size of the ftrace buffers for this instance in the
kernel.  A suffix of 'k', 'K', 'm', 'M', 'g' or 'G' may be
used to specify kilobytes, megabytes or gigabytes.

### modprobe module-name

Force the named module to be loaded by fdr.  This can be
useful when the module is normally loaded on demand and
the probes cannot be enabled until the module is loaded.

### enable subsystem-name/probe-name [filter]

Enable an ftrace probe in the specified subsystem.  Both
the subsystem name and probe name are defined by the kernel.

The optional filter parameter allows an ftrace filter to
be set as well.  This will limit the amount of data being
emitted.  The syntax of the filter language is
defined by ftrace itself and the parameters are defined
by the static tracepoint being enabled in the kernel.

### enable subsystem-name/all

Enable all ftrace probes for the subsystem.

### disable subsystem-name/probe-name

Disable an ftrace probe in the specified subsystem.  This
can be useful to disable selective probes when the "ALL"
keyword has been used.

### disable subsystem-name/all

Disable all probes in the specified subsystem.

### saveto file-name [maxsize]

Save the output of enabled probes to the named file.  If
the optional maxsize parameter is given, the daemon will
initiate a log rotation, see [Log Rotation](README.md#log-rotation) below.
A suffix
of 'k', 'K', 'm', 'M', 'g' or 'G' may be used to specify
kilobytes, megabytes or gigabytes.

If no saveto directive is present, then fdr will create the
instance and enable the probes.  In this case, the data
can be harvested manually by reading:

```
/sys/kernel/tracing/instances/iname/trace_pipe
```

The ftrace buffers in the kernel are circular. If no
process harvests the data, new data will overwrite old data.

### minfree value

Limit the output by the daemon based on free space in the
file system for the save file.  If free space percentage is
below the specified value, no output will be written.

If no minfree directive is present, fdr will use 5% by
default.

## Log Rotation

fdr can use ```logrotate(8)``` to manage the output files.  By convention,
``` /etc/logrotate.d/instance-name ``` controls the behavior of logrotate.

fdr will also invoke logrotate directly at startup and when reaching
the maxsize limit for the save file.

## See Also

[trace-cmd](https://lwn.net/Articles/410200/)

[ftrace documentation](https://docs.kernel.org/trace/ftrace.html)

## Kubernetes

FDR runs on each cluster node as a privileged DaemonSet. Two deployment paths
are provided:

| Method | Path |
|--------|------|
| Plain manifests + kustomize | [deploy/kubernetes/](deploy/kubernetes/) |
| Helm chart (node selectors, values) | [deploy/helm/fdr/](deploy/helm/fdr/) |

```sh
# Build locally
docker build -f deploy/kubernetes/Dockerfile -t fdr:latest .
kubectl apply -k deploy/kubernetes/

# Or Helm
helm upgrade --install fdr ./deploy/helm/fdr \
  --namespace fdr-system --create-namespace \
  --set image.repository=ghcr.io/<owner>/fdr
```

Push tagged releases to GHCR via `.github/workflows/publish-image.yml`.

## Source layout

The daemon is split into focused translation units under `src/`:

| File | Responsibility |
|------|----------------|
| `main.c` | CLI, daemon foreground mode |
| `config.c` | Parse `/etc/fdr.d/*.conf` |
| `trace.c` | ftrace instances, probes, modules |
| `harvest.c` | trace_pipe reader, rotation, disk limits |
| `process.c` | Workers, signals, shutdown |
| `util.c` | Shared helpers |
| `fdr.h` | Types and shared API |

This keeps each concern testable and easier to extend (for example, adding
eBPF or alternate backends later) without a single 800-line file.

## Building & Installing

```sh
make
sudo make install
sudo systemctl enable --now fdr
```

Requirements: a C compiler (`gcc`), `make`, and `install` from coreutils.

### Development

Validate configuration parsing without root or a running tracefs mount:

```sh
make check
```

This runs `fdrd -n`, which parses config files and exits. Use `-c` to point
at a custom config directory and `-v` for verbose output.

Continuous integration runs the same checks on every push via GitHub Actions.

## Kubernetes

FDR can run on every cluster node as a privileged DaemonSet that mounts host
tracefs and writes logs to a host directory. See [deploy/kubernetes/README.md](deploy/kubernetes/README.md)
for manifests, a Dockerfile, and deployment steps.

```sh
docker build -f deploy/kubernetes/Dockerfile -t fdr:latest .
kubectl apply -k deploy/kubernetes/
```

## Contributing

This project welcomes contributions from the community. Before submitting a pull request, please [review our contribution guide](./CONTRIBUTING.md)

## Security

Please consult the [security guide](./SECURITY.md) for our responsible security vulnerability disclosure process

## License

This repository is licensed under the Universal Permissive
License (UPL). See [LICENSE.txt](./LICENSE.txt) for the full text.
