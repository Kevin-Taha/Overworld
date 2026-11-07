# Contributing

This is a native Windows project. Read the [README](README.md) and
[architecture](docs/ARCHITECTURE.md) before proposing changes. Original code and
documentation use the [MIT License](LICENSE); preserve the separate licenses and
attribution for third-party material, especially the CC BY-SA vocabulary data.
Submit only contributions you have the right to provide under the applicable
license. No separate contributor license agreement is required.

## Reports and proposals

Search existing issues and [known limits](docs/TROUBLESHOOTING.md) first.
For bugs, include the build version, architecture/flavor, controller type,
minimal reproduction, expected/actual behavior, and whether physical takeover
and emergency pause still work. Keep feature proposals focused on a real user
scenario and the product boundaries in [the roadmap](docs/PLAN.md).
Sensitive vulnerability reports belong in the [security process](SECURITY.md),
not public issues, pull requests, or attachments.

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

## Development component harness

`OverworldPractice` is an explicitly built development/demo target, not part
of the shipping app or release packaging. See the
[component map and launch instructions](docs/PRACTICE-HARNESS.md).

When changing a component demonstrated there, link the same production library
and update its harness scenario, deterministic contracts, and render checks in
the same change. Do not maintain parallel keyboard widgets, character layouts,
pointer response curves, or cursor artwork. Shared component edits must appear
in both executables after rebuilding. The test world and its local input sink
are fixtures, not a second implementation of desktop delivery or a product
onboarding flow.

For new product features, explicitly state whether the harness exercises the
real component, a bounded fake boundary, or neither. Add a relevant fixture and
update its component map where safe; document unsupported external delivery,
capture, or OS interactions rather than adding them to the harness. Preserve
its reader shutdown, handoff, local hold/page, and no-output contracts. Do not
add the development executable to the default build, product CLI, or releases.

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

Review new files as well as the diff: `.gitignore` does not remove files already
tracked or erase history. Preserve third-party attribution and include `LICENSE`
with distributions. See [Publication readiness](docs/PUBLISHING.md) for history,
visibility, and release checks. Release assets are loose files, not ZIPs.
