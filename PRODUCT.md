# Product

## Register

product

## Users

Linux systems engineers, SREs, and platform engineers who deploy FDR on hosts
or Kubernetes nodes. They use its operational surfaces while validating a
deployment, responding to an incident, or deciding whether a kernel capture is
complete enough to trust.

## Product Purpose

FDR continuously preserves bounded Linux kernel tracing evidence for failures
that are difficult to reproduce. Its dashboard should answer three questions
quickly: is every recorder operating, is trace data being retained without
loss, and where should an operator investigate first?

## Brand Personality

Calm, precise, forensic. The product should communicate technical confidence
without pretending that degraded or incomplete evidence is healthy.

## Anti-references

Avoid neon cyber-security styling, decorative gauges, unexplained color,
overloaded network-operations-center screens, and dashboards that optimize for
visual density instead of incident decisions. Avoid hiding loss behind an
overall green status.

## Design Principles

1. Put evidence integrity before throughput vanity metrics.
2. Make healthy, degraded, and incomplete capture states explicit in words and
   numbers.
3. Preserve Grafana conventions so experienced operators can navigate without
   learning a custom interface.
4. Lead from fleet status to the affected pod and then to the responsible
   counter.
5. Keep every panel actionable during an incident.

## Accessibility & Inclusion

Target WCAG AA contrast. Never communicate state through color alone. Use
color-blind-safe status colors, visible labels, meaningful panel titles, and
stable layouts without decorative motion.
