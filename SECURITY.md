# Reporting security vulnerabilities

Please do **not** open a public GitHub issue for security vulnerabilities.

If you believe you have found a security issue, report it privately through
GitHub's [security advisory workflow](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)
for this repository, or contact the maintainers directly if that option is
unavailable.

Include a clear description of the issue, steps to reproduce it, and the
impact you believe it has.

Non-vulnerability security suggestions (hardening ideas, threat-model notes,
and similar feedback) are welcome as regular GitHub issues.

## Security-related information

FDR controls host-kernel tracefs and normally runs as root. The Kubernetes
DaemonSet is privileged. Write access to configuration, workload arguments, or
the container image must therefore be limited to trusted administrators.
Compromise of the pod must be treated as compromise of the node: privileged
execution and writable tracefs are intentional capabilities, not a container
isolation boundary.

The base Kubernetes deployment does not expose host <code>/lib/modules</code>.
The Helm chart can mount it read-only when <code>modules.enabled=true</code> for
configurations that use <code>modprobe</code>. Module loading changes the host
kernel and must remain disabled unless it is a reviewed requirement.

The HTTP endpoint has no authentication or TLS and binds to loopback by
default. Kubernetes manifests bind it to the pod interface for health probes but
do not create a Service. Do not expose it to an untrusted network without an
authenticated proxy or equivalent network policy.
The Helm chart's optional NetworkPolicy limits ingress only when the cluster
network plugin enforces NetworkPolicy; it is not application-layer
authentication.

Output files are opened without following a final symlink and must be regular
files. Configuration still selects host paths and tracepoints, so configuration
files must not be writable by untrusted users.

Review enabled probes for performance and data sensitivity before production
use. Sample configurations are examples, not universal safe defaults.
