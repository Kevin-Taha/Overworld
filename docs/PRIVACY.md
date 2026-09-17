# Privacy and data handling

Gamepad Desktop is a local desktop utility. It does not implement telemetry,
cloud dictation, transcript uploads, or automatic model downloads.

## Data used during operation

- Controller state and monotonic timestamps determine deliberate input.
- Native window/process identity, session/integrity metadata, routing/capture
  state, and physical-input observations protect input delivery.
- Settings store pointer tuning, exclusions, keyboard/filter options, and optional
  voice enablement/model path in `%LOCALAPPDATA%\GamepadDesktop\settings.ini`.
- Live keyboard edits use bounded pending queues. Previews keep a bounded local
  sample, not a persistent draft.
- Voice holds bounded audio and recognition results in process memory and does
  not write them to files. Cancellation prevents stale insertion; it is not a
  promise of forensic memory erasure or control over operating-system crash dumps.

Microphone access requires opt-in, a deliberate held control, a valid original
target, and an unexpired capture lease. Windows microphone privacy/device settings
remain authoritative and are not changed by the app.

## Diagnostics

Details retains bounded status and category-level failure information rather
than typed text or audio. Native diagnostics may include executable names, window
identifiers, process/thread identifiers, build identity, and gate/query metadata.
Configured paths and captured screenshots can also reveal personal information.
Review and redact all diagnostics before sharing them.

The source tests use synthetic values and public fixtures. Test output, generated
frames, compiler paths, CMake caches, PDBs, dumps, recordings, and package manifests
from an unrelated repository must not be copied into a source publication.

## Network boundaries

Optional developer dependency/model preparation scripts fetch public pinned
artifacts over HTTPS with hash checks. This is a build-time action, separate from
application startup and Settings Save. Offline builds can reuse trusted verified
static-library packages and locally supplied model bytes.

No crash-safety or universal input-target guarantee is implied. If an app keeps
multiple semantic fields inside one native control, its internal field changes
may not be observable through the supported identity checks.
