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

## Preview-only word suggestions

The default-off `--prediction-preview` experiment is confined to its own visible
in-memory practice sample. It cannot deliver text to another application, and
normal application/inspection modes do not collect prediction prefixes. All
external targets are ineligible, including known password fields and unknown
protected/custom controls. This is deliberate scope restriction, **not password
detection** or a claim that HWND identity identifies a safe semantic field.

Only completed practice-keyboard edits feed a bounded ephemeral prefix. The
authored generic word table is fixed: no learning, personal-word-list reads,
OS dictionary access, global UIA value reads, clipboard access, application text
scraping, model, network service or history is involved. The predictor does not
receive dictation. Preview focus loss and physical key/mouse messages reset
practice; device/context/mode/cancellation boundaries revoke suggestions. Nothing
is persisted or logged, and the preview does not open the microphone.

Do not type secrets into the explicitly visible practice field. Like existing
previews it displays its own bounded sample, and process memory is not promised
to be forensically erased. Synthetic demo frames contain authored test text only.
Transport-completed edits are assistance hints, not authoritative application
contents or proof of receipt.

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
