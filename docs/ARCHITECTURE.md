# Architecture

Gamepad Desktop is one native Windows desktop executable with small testable
modules. Application code uses MSVC C++20. The controller poller, recognizer
worker, and optional navigation-hook thread have bounded, distinct responsibilities;
input delivery and its ownership ledger remain serialized on the UI thread.

## Module map

| Area | Implementation | Responsibility |
| --- | --- | --- |
| Controller sampling | `XInputReader.*`, `ControllerPoller` | Latest timestamped XInput snapshot; no input output |
| Pointer model | `Pointer.*`, `Live.*` | Movement, buttons, handoff, mode state, accepted-output ownership |
| Native boundary | `Desktop.*` | Session, integrity, focus, routing, physical-input checks and tagged `SendInput` |
| Cursor | `CursorPresentation.*`, `CursorRing.*`, `NativeCursor*` | Layered pixels and ownership-safe native hiding |
| Keyboard | `Keyboard.*`, `KeyboardDelivery.*`, `TextDelivery.*` | Selection, explicit edits, original-target delivery and recovery |
| Keyboard visuals | `KeyboardVisual.*`, `KeyboardPopup.*`, `KeyboardPreview.*` | Nonactivating alpha popup and no-output previews |
| Word suggestions | `WordPrediction.*`, `PredictionTarget.*`, `PredictionPractice.h` | Bounded frequency-ranked hints, opt-in native metadata proof and in-memory practice |
| Voice | `Voice.*`, `NativeVoice.*`, `Audio*`, `VoiceModel.*`, `VoiceRecognizer.*` | Bounded capture, validation, local inference, cancellation |
| Voice view | `VoicePresentation.h`, `VoiceViewAdapter.h` | Backend-independent phase/level/hint snapshots |
| App UI | `Host.*`, `AppUi.*`, `Resources.h` | Native dashboard, modeless settings, status, tray, lifecycle |
| Persistence | `Settings.*` | Strict schema parsing and transactional saves |
| Optional filter | `NavigationFilter.*`, `NativeNavigationFilter.*` | Pure key ledger plus bounded native hook adapter |
| Own caption | `SelfCaption.h` | Deferred self-window min/max/close after a processed matching release |
| Simulator | `Core.*` | Shared snapshot types and retained dry-run model; not a second output boundary |

## Input authorization and ownership

Controller activity is acquired only after fresh neutral and deliberate input.
Ambiguous simultaneous devices do not merge gestures. Raw physical input, hard
session/token failures, explicit exclusions, stale data, and recovery obligations
take precedence over new output.

Every accepted synthetic down is tracked until its matching up is accepted.
Cleanup is independent of whether new actions are enabled, but it still requires
a trusted release route and must not interfere with a physical hold. Failed or
uncertain cleanup remains visible and prevents new input. A mode change does not
erase this ledger.

Use the same monotonic clock for sampling, delivery, and lease validation.
A delayed consumer must not replay missed motion or button edges. Narrowly
classified disappearing-window queries may defer an already-active pointer for
at most 100 ms with **zero output** until full revalidation; they do not authorize
new acquisition or text. The original deadline survives downstream dispatch.

Automatic game switching combines documented notification state with foreground
fullscreen pointer confinement. A manual desktop request can override recognized
heuristics for the current root-owner activation, not security failures or
exclusions. It is temporary, not persistent app classification.

## Cursor presentation

The custom cursor publishes its image before hiding the native cursor. It restores
only hiding owned by this process before dropping that image. A failed restore
retains the last overlay and recovery obligation. Explicit user recovery is a
separate path; automatic fallback must not force-show an unowned cursor.

Start fallback matches a validated visible, non-iconic, uncloaked
`StartMenuExperienceHost.exe` / `Windows.UI.Core.CoreWindow` foreground or hit-test
root. Two bounded identity caches avoid per-frame process-image lookup. Unknown
observations retain an established fallback. This is presentation only and does
not grant input permission or establish compositor-band ordering.

Own tagged caption-button clicks avoid a native tracking loop that can starve the
timer responsible for their release. Consuming a known synthetic caption message
is distinct from authorizing an action. A command needs the matching processed
up, a released ledger, valid controller/window identity, and the same button hit.
Physical mouse handling, caption dragging, and other apps are not rewritten.

## Keyboard and text

`Daisywheel` is independent of pointer mode and native output. Face-button mode
uses eight groups with physical A-bottom/B-right/X-left/Y-top slots. One-handed
mode commits a group on recenter, then a cardinal character on the next recenter.
Its pending group/candidate is separate from its committed selection. Action
sentinels are rendered as symbols and are never delivered as literal text.

The live path emits Character, Backspace, and Enter edits immediately through a
bounded queue. It does not maintain a private draft; a bounded local sample exists
only for explicit previews. Menu release plus an edit in one snapshot is ordered
without dropping either event. View and cancellation take precedence.

`TextDelivery` validates original native/semantic focus and accounts for individual
accepted key events. Own accepted-up settling can defer a bounded edit, not relax
its target or deadline. Focus notifications are filtered for relevance and
deduplicated; real field changes and foreground excursions still invalidate the
target. Caret blinking is not routing identity. HWND identity cannot establish
internal field identity in every browser or custom control.

### Scoped native word suggestions

`TextBoundary::prediction_enabled()` is off by default. Runtime
`--word-prediction` enables native availability, but collection still requires
an isolated neutral-then-500-ms D-pad Down hold in an eligible original field for
each keyboard session. `PredictionPractice` remains a separate no-output sink.
Both use `KeyboardDelivery` and `TextDelivery` rather than a second output path.

The native first slice accepts only visible/enabled/writable child windows for
which both class name and real class are exactly system `Edit`, with
`ES_MULTILINE` and without `ES_PASSWORD` or `ES_READONLY`. System multiline Edit
does not support password characters; single-line Edit, RichEdit, browsers,
modern Notepad and unknown/custom classes are excluded. These metadata checks
cannot prove that an app has not internally subclassed an Edit. Explicit
non-secret field trust is required and there is no universal password guarantee.

A single latest-request worker reads only scalar `WM_GETTEXTLENGTH`, `EM_GETSEL`,
and scalar length again. Each cross-thread message has a maximum 20-ms timeout;
the entire batch has one 60-ms deadline and stop checks. No text buffer or
cross-process caller-owned pointer is sent. Lengths must be equal and below
65,535 code units; selection must be collapsed and within that length. The length
guard is necessary: actual classic Edit controls can wrap a 70,000 index to
4,464 in the packed `EM_GETSEL` result instead of returning the documented -1.
Large fields therefore fail closed, even with the caret near the beginning.

Results carry the original native target, output revision, close generation and
original monotonic sample time. Stale/replaced results never authorize hints.
The input thread only reads the mailbox and window metadata; the worker never
calls stateful `Desktop::observe` or performs input. After accepted events, a
16-ms settling interval precedes a new query so sent metadata messages do not
immediately overtake queued input. This is not proof of application receipt.

`TextDelivery::pump` returns at most four fully completed edits per eight-event
budget. A partial edit or stopped/reentered delivery is not a prefix update.
The predictor stores at most 16 ASCII letters and three candidates from the
separately licensed, embedded 10,000-word frequency list. Native trust starts an
empty known stream immediately: two own letters suffice without leading Space,
without claiming knowledge of preceding field content. Space/punctuation reset
the prefix; overflow, unknown Backspace and non-ASCII boundaries suppress it.
Lowercase, title case and uppercase are preserved. There is no native draft,
inferred Backspace, text-value read, learning or Unicode segmentation framework.

D-pad selection/acceptance requires a fresh neutral sample, no other controls,
the one-handed Group stage and an empty delivery queue. Acceptance consumes a
generation and enqueues only the validated remaining suffix. Native completions
have one fixed one-second deadline from acceptance; each new character down
requires a fresh proof of its expected caret and original target, while accepted
ups retain independent ownership accounting. Preview completions retain the
original 100-ms deadline. Pending suffix work never feeds itself back into
prediction. No Space, Enter or replacement is implicit.
Cancellation, target/context/activity/device changes and voice transitions clear
the hint and revoke acceptance/trust; dictation is not a prediction source.
Unexplained caret/selection changes hide and clear hints rather than reconstruct
field text. Pending own edits have at most 100 ms to obtain a matching caret.
For suffix delivery, both dispatch guards share an explicit record of the caret
before an accepted own down and its expected advancement. Only that known old
caret can defer the next down, for at most 100 ms from the accepted down and
never beyond the original completion deadline. Key-up cannot renew this wait.
A fresh advanced-caret observation consumes the allowance, including observations
during key-up or in the keyboard's outer validation; a later rollback is rejected.
Timeout cancels the unsent suffix explicitly and never replays accepted events.
Length/caret stability is not proof of unchanged content or atomic insertion;
unobservable same-length programmatic replacements remain a documented limit.

The optional 66-DIP suggestion strip is outside the original wheel/footer. Its
owning view/cache is bounded, validates actual label fit, and hides chips when
inactive, voice-active or without a candidate. Normal keyboard dimensions and
pixels without the option are unchanged. `--prediction-demo` seeds only synthetic keyboard snapshots
and does not start controller polling.

## Local voice

Voice is explicitly enabled and begins only with held right-stick click while
the keyboard owns a valid target. Capture authorization is a same-clock lease,
renewed by fresh held-button input. An expired generation cannot be revived.
Target/context/device/mode changes, physical takeover, and close cancel work and
reject late results.

WASAPI capture and recognition run off the input/UI thread. Audio is bounded,
normalized to the recognizer's format, and never written by the production path.
Only the released tiny.en model with the pinned SHA-256 is accepted. The exact
verified bytes are passed to Whisper; the parser does not reopen an unchecked file.
Recognition output is bounded and inserted as text into the original target,
without command interpretation, automatic Enter, or replay.

Application code stays on MSVC. ARM64 Whisper/ggml uses a separate clang-cl
static-library project with Microsoft's ABI and matching Release `/MT` CRT.
Import validation checks architecture, source pin, compiler compatibility, and
all package hashes. Missing requested dependencies fail configuration. This is a
build boundary, not a runtime plugin or cloud service.

## Rendering and settings

The keyboard uses Direct2D/DirectWrite on reusable WIC premultiplied BGRA surfaces,
presented via `UpdateLayeredWindow`. Grayscale text antialiasing works with alpha.
Backgrounds precede letters; actual glyph bounds are tested for nonoverlap and
material coverage. Unchanged state does not rerasterize. Idle voice leaves the
center empty; active phase/level/hint rendering owns any retained string data.

App/settings controls remain native HWND controls with tab navigation, accessible
names, DPI scaling, app light/dark preference, and system high-contrast colors.
Theme reads are cached and invalidated by system notifications, never written.

Settings measures the resource font's 96-DPI dialog units before replacing its
fonts, then scales that fixed client/control layout. Native dialog-unit rounding
at the creation monitor must not inflate the logical window on higher-DPI
displays. The body and title remain 14 and 26 DIP. Initial placement and DPI
changes calculate the complete nonclient frame at the target DPI and keep it
inside the monitor work area; an unusably small work area is an explicit layout
failure, not a reason to shrink fonts or silently clip footer actions.

Settings schema 11 adds default-off voice preferences to existing pointer, keyboard,
exclusion, and filter settings. Earlier supported schemas are read without writes;
missing/duplicate/unknown fields are rejected according to their version.
Explicit Save validates, detects external changes, and preserves the old state
on failure. Configuration changes cancel current interactions and require fresh
input; they do not silently clear manual pause or output faults.

## Optional navigation filter

The pure 24-key ledger covers injected `VK_GAMEPAD` C3..DA only. Unknown first
holds pass; repeats and ups follow the original down. Source ambiguity quarantines
that key rather than guessing device identity. Ordinary/physical input and the
application's own tagged output pass through.

The hook callback consumes a bounded-age foreground policy and performs no
allocation, locking, I/O, process opening, or input injection. Scope loss drains
matching holds for a bounded interval; shutdown fails open and does not block
indefinitely. Identical sources and silent OS hook removal remain limitations.
Inspection and preview modes do not install the hook.

See [Testing](TESTING.md) for evidence boundaries and
[Privacy](PRIVACY.md) for data-handling expectations.
