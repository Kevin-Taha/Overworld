# Contributing

This is a native Windows project. Read the [README](README.md) and
[architecture](docs/ARCHITECTURE.md) before proposing changes. The absence of an
application license does not grant redistribution or reuse rights; preserve all
existing third-party notices.

## Development workflow

1. Use a dedicated checkout or branch. Keep generated files under `out` and do not
   overwrite an executable that someone is running.
2. Build from the application root using [Building](docs/BUILDING.md).
3. Start with a focused regression test or an explicit reproduction. Keep changes
   small and avoid mixing interaction changes with rendering or dependency updates.
4. Run the smallest relevant deterministic tests, then broaden validation when
   needed. Use [Testing](docs/TESTING.md) for optional native probes.
5. Describe the behavior changed, commands run, and remaining device/runtime
   limitations. Do not claim microphone or application compatibility from a build.

## Safety and privacy

Do not automatically launch the live mapper, install its optional hook, record a
microphone, inject input, manipulate cursor visibility, or modify OS/user settings
as part of a build or default test. Interactive checks need explicit authorization
and disposable owned targets. Preserve normal mouse/keyboard takeover and all
accepted-output cleanup obligations.

Do not attach raw audio, transcripts, private window titles, model paths, account
names, process dumps, or screenshots containing unrelated applications.
Use generic fixtures and redacted category-level diagnostics. See
[Privacy](docs/PRIVACY.md).

## Code conventions

- MSVC C++20, `/W4 /WX /permissive- /utf-8`; retain x64 and ARM64 support.
- Reuse pure models and narrow native boundaries. Avoid global hooks, worker
  frameworks, platform services, or dependency additions outside an explicit task.
- Native output and cancellation are serialized. Never replay partial output,
  clear a fault silently, or revive an expired authorization.
- Preserve settings schema compatibility and no-write loading. Add atomic failure
  and old-schema tests for persistence changes.
- Rendering changes need actual glyph/control geometry, alpha, DPI, theme,
  high-contrast, and resource-lifetime checks, not only successful paint calls.
- Only comments explaining non-obvious constraints are useful. Keep diagnostics
  bounded and free of typed content.

## Source and release hygiene

Track source, tests, build scripts, generic documentation, and required notices.
Do not commit executables, PDBs, toolchains, dependency caches, model weights,
recordings, fixture downloads, logs, or user configuration.

When creating a new standalone repository from a source snapshot, copy only the
approved application files. Do not copy an enclosing repository's `.git`,
configuration, hooks, unrelated directories, or history. Review the complete
snapshot before publishing it. Release assets are loose files, not ZIPs.
