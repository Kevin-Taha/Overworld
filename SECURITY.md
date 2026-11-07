# Security policy

Overworld is early-development software. Security fixes target the current
`main` branch; no support or backport commitment is made for older snapshots or
experimental branches. Build identity is not a code signature or a security
certification.

## Report a vulnerability privately

Use GitHub's **Security > Report a vulnerability** when private vulnerability
reporting is enabled:
[submit a private report](https://github.com/Kevin-Taha/overworld/security/advisories/new).
If that option is unavailable, do not open a public issue containing sensitive
details. Ask the maintainer to enable private reporting using a generic request
without exploit details, secrets, or personal information.

Include the affected commit/build, architecture and voice flavor, expected
security boundary, a minimal reproduction, impact, and any proposed mitigation.
Use synthetic data and disposable owned targets. Do not include credentials,
private window titles, settings/model paths, raw audio, transcripts, dumps, or
screenshots of unrelated applications. Do not run native input, microphone,
hook, or cursor-visibility probes without explicit authorization.

Please allow private triage and coordination before disclosing sensitive
details. There is no guaranteed response time or bounty program.

## Boundaries worth reporting

Examples include input reaching a different target after cancellation, physical
takeover failing, replay of partially accepted input, unsafe accepted-key
cleanup, unauthorized recording, expired capture authorization being revived,
or unverified model bytes reaching the recognizer. Ordinary controller mapping
problems and known app/OS limitations belong in redacted bug reports.
See [Architecture](docs/ARCHITECTURE.md) and [Privacy](docs/PRIVACY.md).
