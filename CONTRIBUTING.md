# Contributing to this repository

Contributions are welcome. There are several ways to help.

## Opening issues

For bugs or enhancement requests, please file a GitHub issue unless the report
is security related. When filing a bug, include steps to reproduce, expected
behavior, and what actually happened.

If you think you have found a security vulnerability, do not open a public issue.
Follow the instructions in [SECURITY.md](./SECURITY.md).

## Contributing code

1. Fork this repository.
2. Create a branch for your change.
3. Make your changes and update documentation if needed.
4. Run <code>make check</code>, <code>make sanitize</code>,
   <code>helm lint deploy/helm/fdr</code>, and
   <code>kubectl kustomize deploy/kubernetes</code>.
5. Commit with a clear message. Signed-off commits (<code>git commit -s</code>) are
   appreciated but not required.
6. Open a pull request that explains what changed and why, and how to verify it.

## Code of conduct

Be respectful and constructive. The [Contributor Covenant Code of Conduct][COC]
is a good reference for expected behavior.

[COC]: https://www.contributor-covenant.org/version/1/4/code-of-conduct/
