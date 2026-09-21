# Agent instructions

Read [README.md](README.md), [CONTRIBUTING.md](CONTRIBUTING.md), and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) before changing behavior.
Build and test commands are in [docs/BUILDING.md](docs/BUILDING.md) and
[docs/TESTING.md](docs/TESTING.md). Run commands from this application's root,
whether it is a standalone checkout or nested in another repository.

For a new machine or session, inspect Git state, host architecture, and installed
build inputs. Recreate the default deterministic baseline before changing behavior.
Read [docs/PLAN.md](docs/PLAN.md) for declined and parked experiment decisions;
select one small reviewable next step rather than resuming experiments implicitly.

## Product boundaries

- Product name is Overworld. Preserve the legacy persisted names in
  `src/Compatibility.h` and the version-1 Whisper package schema when rebranding.
- Keep one native Windows application. Preserve existing controller mappings,
  pointer defaults, one-handed selection, and accessible native settings controls
  unless a task explicitly changes them.
- Practice is a development-only component harness, not a product mode. Keep
  `OverworldPractice` excluded from the default build and release assets.
  Never add its launch route or modules to the shipping `Overworld` target.
- Mouse and keyboard takeover, session/integrity checks, exclusions, original text
  target identity, and pending-output recovery are safety boundaries, not
  compatibility obstacles to bypass.
- Never steal focus, replace clipboard contents, force another application's
  cursor visibility, or replay uncertain/partially accepted input.
- Restore only native cursor hiding owned by this application before concealing
  its overlay. Failed restoration retains ownership and explicit recovery.
- Local voice is opt-in and push-to-talk. Preserve capture authorization expiry,
  cancellation generations, verified tiny.en bytes, bounded work, and original
  target insertion. No cloud fallback, runtime downloads, background recording,
  transcript logging, automatic Enter, or command interpretation.
- The optional navigation filter only covers the documented injected
  `VK_GAMEPAD` range. Do not broaden it to physical/ordinary keys, add game hooks,
  or weaken mixed-source handling.
- Rendering must remain independent of input and microphone work. Preserve DPI,
  high contrast, premultiplied alpha, glyph clearance, and idle resource reuse.
  A successful paint call is not evidence that a user can see the overlay.

## Engineering

- Make focused changes, reuse existing boundaries, and retain meaningful safety
  regressions. Read local instructions before editing; preserve unrelated work.
- Product model/rendering changes must flow into the development harness through
  the same compiled libraries, not copied widgets, alphabets, motion curves, or
  cursor artwork. For relevant new features, update the harness scenario,
  component contract tests, and `docs/PRACTICE-HARNESS.md` in the same change.
  If a feature cannot run there safely, document it as not exercised and add a
  bounded fake fixture where appropriate; never imply external delivery, voice,
  or native safety checks were simulated successfully.
- Preserve the harness's shared mapper guard, reader's per-slot/original-lease
  checks and terminal bounded shutdown, takeover timestamp fences, matching
  Space-release cleanup, and one-handed local page retention.
- Application code uses MSVC C++20 with `/W4 /WX`. Keep clang-cl isolated to the
  pinned Whisper/ggml dependency build, with compatible architecture and static
  CRT. Requested but unavailable voice dependencies must fail explicitly.
- Settings parsing is strict and transactional. Older schemas load without
  writes; explicit Save validates and detects concurrent file changes.
- Use one monotonic clock domain for deadlines. Do not treat delayed consumers as
  fresh input, revive expired authorizations, or hide errors with success defaults.
- Do not change system input, speech/privacy, theme, security, or startup settings.
  Do not add services, drivers, elevation workarounds, or external UI frameworks.
- Do not add personal paths, account identifiers, internal URLs, raw diagnostics,
  session transcripts, or generated build artifacts to source.

## Validation and handoff

- Start with the default deterministic tests. They do not run the live mapper,
  open the microphone, install the native hook, or inject input.
- Interactive app, microphone, native-input, and cursor-visibility tests require
  explicit authorization. Use disposable owned windows and bounded execution;
  never type into a user's existing documents.
- Render-only and owned-message probes are separate opt-in targets. Respect their
  invocation requirements and watchdogs; never add them to default CTest.
- Do not delete active build trees, model caches, release assets, or repro
  evidence merely to tidy a checkout. Remove only identified, authorized paths.
- Report files changed, commands/results, and what was not exercised. Separate
  compilation, fake tests, prerecorded inference, and real-device validation.
- Releases are loose application files with the verified model and required
  licenses, not ZIP archives. Do not add an application license without an
  explicit ownership/licensing decision.
