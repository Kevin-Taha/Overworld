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

## Opt-in word suggestions

Normal launch and `--inspect` do not collect prediction prefixes. Runtime
`--word-prediction` permits the scoped native pilot, but each keyboard session
also requires a deliberate D-pad Down hold to trust the original non-secret
field before collection. Only system-style classic multiline Edit controls with
no password/readonly style are eligible. Single-line Edit, RichEdit, browsers,
modern Notepad and unknown/custom controls are excluded. Internal subclassing
cannot be universally detected: never trust a field used for secrets. This is
**not universal password detection**.

The metadata worker requests only scalar selection indices and field length,
never text contents or text buffers. Stable length below 65,535 code units is
required to reject actual packed-selection wraparound; all larger fields are
unavailable. Queries are bounded to 20 ms each and 60 ms for a batch. Metadata
failure does not disable ordinary typing. Same-length programmatic text changes
can be unobservable; metadata does not prove receipt, safe application behavior
or atomic delivery.

Only this keyboard's fully transport-completed edits feed a maximum 16-letter
ephemeral prefix. The fixed 10,000-word frequency list is embedded public
CC BY-SA 4.0 data, not personal history. There is no learning, personal-word-list
read, OS dictionary access, global UIA value read, clipboard access, text scraping,
ML, network service or transcript source. Cancellation, unknown Backspace,
physical takeover, caret/selection/target/context/device/mode changes and voice
transitions clear or invalidate hints. Nothing is persisted or logged.

`--prediction-preview` remains a separate visible in-memory practice sample. It
cannot deliver text to another application or open a microphone. Preview focus
loss and ordinary keyboard/mouse messages reset practice; reserved controller
and navigation key messages do not masquerade as physical-key takeover.

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
