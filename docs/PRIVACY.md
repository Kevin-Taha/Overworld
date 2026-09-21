# Privacy and data handling

Overworld is a local desktop utility. It does not implement telemetry,
cloud dictation, transcript uploads, or automatic model downloads.

## Data used during operation

- Controller state and monotonic timestamps determine deliberate input.
- Native window/process identity, session/integrity metadata, routing/capture
  state, and physical-input observations protect input delivery.
- Settings store pointer tuning, exclusions, keyboard/filter options, and optional
  voice enablement/model path in `%LOCALAPPDATA%\GamepadDesktop\settings.ini`.
  This legacy directory is retained by Overworld to preserve existing preferences.
- Live keyboard edits use bounded pending queues. Previews keep a bounded local
  sample, not a persistent draft.
- Voice holds bounded audio and recognition results in process memory and does
  not write them to files. Cancellation prevents stale insertion; it is not a
  promise of forensic memory erasure or control over operating-system crash dumps.

Microphone access requires opt-in, a deliberate held control, a valid original
target, and an unexpired capture lease. Windows microphone privacy/device settings
remain authoritative and are not changed by the app.

## Local word suggestions

Word suggestions are on by default for eligible controller-keyboard sessions.
**Settings > Typing > Word suggestions** provides an opt-out; Save persists only
that preference, never typed fragments. No per-field consent gesture is required.
A fresh Down press hides suggestions for the current session. Inspection mode
does not permit live typing. Browser, Notepad, RichEdit,
single-line and custom targets use the same existing keyboard delivery boundaries;
there is no support allowlist or claim that these targets are verified safe.

Kernel window/class/style metadata supplies negative evidence only: recognized
Edit/RichEdit password styles suppress collection, display and completion.
Failed metadata queries also suppress quietly. Unknown field
privacy, including custom/browser password fields not exposed by these native
styles, cannot be verified. This is **not universal password detection**.
The optional feature does not disable ordinary typing.
No metadata worker, target messages, caret, selection, field-length or text
reads remain. Programmatic caret/selection changes invisible to the existing
target checks may affect where an app applies input, just as for manual
typing/voice; neither transport acceptance nor candidate display guarantees
receipt, field identity, unchanged contents or atomic insertion.

Only this keyboard's fully transport-completed edits feed a maximum 16-letter
ephemeral prefix. The fixed 10,000-word frequency list is embedded public
CC BY-SA 4.0 data, not personal history. There is no learning, personal-word-list
read, OS dictionary access, global UIA value read, clipboard access, text scraping,
ML, network service or transcript source. Cancellation, unknown Backspace,
physical keyboard/mouse navigation, takeover, observed target/context/device/mode changes and voice
transitions clear or invalidate hints. Nothing is persisted or logged.

Development fixtures use a separate bounded in-memory sample. They cannot
deliver text to another application or open a microphone and are not shipping
product modes. Ordinary keyboard/mouse fixture messages reset the sample;
reserved controller/navigation messages do not masquerade as physical takeover.

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
