# Architecture

Gamepad Desktop is one native Windows desktop executable with small testable
modules. Application code uses MSVC C++20. The controller poller, recognizer
worker, and optional navigation-hook thread have bounded, distinct responsibilities;
input delivery and its ownership ledger remain serialized on the UI thread.

## Module map

| Area | Implementation | Responsibility |
| --- | --- | --- |
| Controller sampling | `XInputReader.*`, `NativeControllers.*`, `ControllerMapping.*` | Shared XInput and mapped DirectInput reader; no desktop output |
| Pointer model | `Pointer.*`, `Live.*` | Movement, buttons, handoff, mode state, accepted-output ownership |
| Native boundary | `Desktop.*` | Session, integrity, focus, routing, physical-input checks and tagged `SendInput` |
| Cursor | `CursorPresentation.*`, `CursorRing.*`, `NativeCursor*` | Layered pixels and ownership-safe native hiding |
| Keyboard | `Keyboard.*`, `KeyboardDelivery.*`, `TextDelivery.*` | Selection, explicit edits, original-target delivery and recovery |
| Keyboard visuals | `KeyboardVisual.*`, `KeyboardPopup.*`, `KeyboardPreview.*` | Nonactivating alpha popup and no-output previews |
| Word suggestions | `WordPrediction.*`, `PredictionTarget.*`, `PredictionPractice.h` | Bounded frequency-ranked fragments, explicit suffix append, negative password metadata and practice |
| Voice | `Voice.*`, `NativeVoice.*`, `Audio*`, `VoiceModel.*`, `VoiceRecognizer.*` | Bounded capture, validation, local inference, cancellation |
| Voice view | `VoicePresentation.h`, `VoiceViewAdapter.h` | Backend-independent phase/level/hint snapshots |
| App UI | `Host.*`, `AppUi.*`, `Resources.h` | Native dashboard, modeless settings, status, tray, lifecycle |
| Persistence | `Settings.*` | Strict schema parsing and transactional saves |
| Optional filter | `NavigationFilter.*`, `NativeNavigationFilter.*` | Pure key ledger plus bounded native hook adapter |
| Own caption | `SelfCaption.h` | Deferred self-window min/max/close after a processed matching release |
| Simulator | `Core.*` | Shared snapshot types and retained dry-run model; not a second output boundary |
| Practice | `Practice.*`, `PracticeWindow.*`, `PracticeVisual.*`, `PracticeReader.*` | Pure lesson state, owned native scene, scoped observation worker; no desktop output |

## Controller compatibility boundary

The four-slot snapshot and existing XInput mappings are unchanged. XInput has
priority in its original slots. A disconnected slot can host a mapped DirectInput
controller; an XInput error cannot authorize fallback. Device-specific layouts
come from SDL-format mapping data, never a vendor/product allowlist in application
code. Unmapped devices report an explicit error and GUID for configuration.

Enumeration, mapping, and acquisition failures discard input. Device replacement,
source changes and reacquisition advance connection epochs so old neutral samples
cannot authorize a new device. Batch timestamps precede native reads. Production
and Practice use the same compiled reader and mapping; Practice retains per-slot
authorization and terminal bounded shutdown, including native teardown.

DirectInput enumeration and duplicate-interface checks run on a separate
metadata-only worker. The sampler reads its inventory mailbox without waiting;
slow periodic device discovery cannot block a connected controller's 16-ms reads.
Before the first inventory, discovery reports pending, not an invented empty
success. The metadata worker owns its DirectInput interface, never acquires a
device or reads controls, and cannot publish or start another query after stop.
Its 200-ms stop bound retains in-flight state safely on timeout and reports
failure; Practice includes this result in its existing terminal shutdown path.
The keyboard's 100-ms sample-age/continuity checks and original sampling
timestamps are unchanged.

## Development-only practice harness

Practice is not a product mode. `GamepadDesktopPractice` is an explicit
`EXCLUDE_FROM_ALL` target with a separate `PracticeHost.cpp` entry point.
The shipping `GamepadDesktop` target contains no practice modules and its
strict `LaunchMode.h` parser rejects `--practice` and other unsupported flags
before acquiring any instance guard or constructing the live Host.
Both executables use `MapperInstanceGuard` with the existing single-mapper
mutex name; the harness cannot coexist with another live mapper.

The [harness component map](PRACTICE-HARNESS.md) is the coverage contract.
Pointer/Daisywheel, keyboard rendering, cursor pixels/defaults, app appearance,
and controller reading each have one shared-library compilation owner linked by both
targets. `PracticeVisual` composes actual keyboard/cursor renderer pixels rather
than reimplementing their widgets or artwork. Configure-time target contracts,
default tests, and opt-in pixel comparisons guard that relationship.
Practice implementation libraries and their deterministic/render test executables
are also excluded: no default target may transitively pull them into `ALL_BUILD`.

Practice owns only scene state and native child controls. `PracticeWindow` receives
an observation-only source. A reader thread is authorized by a 50 ms lease only
while its own window is active, foreground, non-iconic, and not processing a held
local mouse/key gesture. The reader independently checks foreground identity and
minimization before controller sampling; it does not sample in the background. The UI
consumes only the latest timestamped snapshot and rechecks its own focus. This
keeps device reads independent of rendering. There is no settings load/save,
native output adapter, global physical-input
observer, cursor-hiding ownership, navigation hook, microphone, clipboard, or text
target lookup. Physical takeover is observed through messages to its own windows.
The existing pure key classification is shared through `InputClassification.h`.
Focus return, physical input, and native practice commands fence off snapshots
sampled at or before that transition. An unread mailbox neutral cannot authorize
a held gesture after a handoff merely because it is less than 100 ms old.
The reader rechecks stop, scope generation, actual focus, and the original batch
lease before every slot and before publication. Renewal cannot revive an expired
batch. Shutdown waits at most 250 ms; failure is reported and the practice process
exits instead of restarting. Native controller calls have no hard cancellation: on timeout, the
in-flight call retains worker-owned state until it returns or the process exits.
Terminal stop prevents every subsequent slot, batch, and publication.
Reserved `VK_GAMEPAD` and `VK_NAVIGATION` messages from controller gestures never
enter the physical held-key ledger or cancel a lesson. This is own-window message
handling only, not a global hook or a change to the optional navigation filter.

The pure model integrates the existing Pointer response and precision settings,
scaling the training coordinates to preserve physical-pixel speed. The existing
bounded-preview Daisywheel produces local characters, including one-handed
flicks. Controller ownership, sample age/clock checks, neutral reacquisition,
focus loss, connection epochs, and multiple-controller ambiguity cancel partial
gestures without completing a drop or replaying a click. Same-button down/up
hit-testing calls only local practice commands; it is not another native output
boundary. Help suspends lesson effects while allowing its own buttons and View.
Scene-owned Space releases are handled before child/mode guards; losing scene
focus or changing mode cancels local holds rather than completing a drop.
The local one-handed fallback retains its selected page across button presses
and idle gaps through a preview-only page setter that refuses pinned/live typing.

The complete practice frame is a retained opaque premultiplied WIC/D2D surface
with measured DirectWrite ink. The keyboard is composited from its unchanged
renderer. Standard owner-drawn BUTTON/STATIC children expose accessible names,
tab order, status, and keyboard actions while presenting the same pixels.
The scene's native accessible name also exposes its bounded local typing sample.
Theme reads reuse `AppUi`; no system preference is written. With no controller
source, the window stays hidden/offscreen for the opt-in render-only helper.

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

`--release-tap` is an immutable startup option on the existing `Live` model,
independent of cursor presentation. A stationary short A press is pending until
release; a held 250-ms press or movement becomes a normal drag. The original
application activation, context, route, cursor and geometry stay pinned through
both dispatcher validations. Interruptions discard uncommitted taps. An accepted
tap down remains owned until the next-sample up, allowing processed own-caption
messages to use the existing ledger. Repressing while a drag owns Primary never
sends a second down. Fresh pending-entry validation clears a resolved routing
wait, not the gesture's original timestamp. Default A-down timing is unchanged.

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

The native keyboard popup uses `KeyboardPlacement` to retain its ordinary
bottom-center/current position unless it overlaps the verified foreground Start
or Search window. `NativeKeyboardShell` reads only foreground root identity,
process basename, visibility/cloak and DWM frame bounds, with cached process
identity; it does not query field text, use UIA, or call stateful input routing.
The pure bounded search tries rectangle-edge candidates inside the target
monitor's work area, retaining full DPI-scaled artwork and an 8-DIP shell gap.
Current clear placement wins, then shortest movement with deterministic ties.

Visible sessions run placement preparation before `KeyboardDelivery::step`.
No available rectangle or unavailable geometry blocks the session before another
edit; accepted-key cleanup remains owned by the delivery model. Expansion,
shell movement and DPI changes recheck placement. A placement failure follows
the existing close/new-open recovery rule, never an automatic typing retry.
The layer remains nonactivating/topmost; there is no privileged band, focus
transfer, shell dismissal, or alternate delivery target. Shell frame bounds are
conservative metadata, not a claim of universal compositor visibility.

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

`KeyboardRouting.h` limits the native-mode opener's mouse-route observation to a
closed keyboard. `Desktop::observe` is stateful: a blocked mouse recipient can
advance the shared unsafe-context generation. A visible desktop or native
keyboard therefore keeps its existing mode and validates its original text
target through `KeyboardDelivery`, not an unrelated potential mouse recipient.
Actual text-target, session, integrity, exclusion, physical-input and host
prerequisite failures still cancel typing and retain accepted-up recovery.

### Explicit append-only word suggestions

`TextBoundary::prediction_enabled()` is on by default, controlled by the one
Settings > Typing checkbox. There is no per-field consent gesture. A fresh Down
press hides suggestions for the current session without erasing typed text; its
held tail cannot reenable collection. `PredictionPractice` is a development-test
fixture only, not a shipping launch mode or runtime dependency.
Both use `KeyboardDelivery` and `TextDelivery` rather than a second output path.

The completion path accepts the same otherwise-eligible `TextBoundary` targets
as manual typing/voice. There is no support allowlist, classic-Edit restriction,
caret/length RPC, selection-index limit, metadata worker or application receipt
protocol. `PredictionTarget` now supplies only negative privacy evidence from
kernel window identity/class/style metadata: recognized Edit/RichEdit password
styles and metadata query failure quietly suppress suggestions. Everything else
is `Unknown`, never positive proof of a plain-text field.
Browsers/custom controls can hide password state or semantic-field transitions
inside one HWND; existing manual/voice target guards remain authoritative.

`TextDelivery::pump` returns at most four fully completed edits per eight-event
budget. A partial edit or stopped/reentered delivery is not a prefix update.
The predictor stores at most 16 ASCII letters and three candidates from the
separately licensed, embedded 10,000-word frequency list. Each session starts an
empty fragment immediately: two own letters suffice without leading Space,
without claiming knowledge of preceding field content. Space/punctuation reset
the prefix; overflow, unknown Backspace and non-ASCII boundaries suppress it.
Lowercase, title case and uppercase are preserved. There is no native draft,
inferred Backspace, text/length/caret read, learning or Unicode segmentation framework.

D-pad selection/acceptance requires a fresh neutral sample, no other controls,
the one-handed Group stage and an empty delivery queue. Acceptance consumes a
generation and enqueues only the validated remaining suffix. Native completions
have one fixed one-second deadline from acceptance. Each event uses the same
original-target, physical-input, context and deadline guards as ordinary text,
plus known-protected/unavailable privacy suppression. Accepted ups retain
independent cleanup ownership accounting. Preview completions retain the
original 100-ms deadline. Pending suffix work never feeds itself back into
prediction. No Space, Enter or replacement is implicit.
Cancellation, target/context/activity/device changes and voice transitions clear
the fragment; dictation is not a prediction source. Disabling suggestions clears
the fragment, hints and unsent suffix immediately, including during reentrant
output, but never clears accepted-up cleanup ownership. Ordinary
observed keyboard/mouse navigation invalidates through the existing physical
activity boundary. Reserved controller/navigation key messages are not confused
with physical takeover. Any protected/unavailable observation during an ordinary
edit suppresses the prediction batch even if privacy becomes unknown afterward;
manual typing itself is not disabled by the optional privacy feature.

User-selected completion is a literal append request, not an editor synchronizer:
the app owns its current caret/selection and may apply or discard the input.
The utility never intentionally replaces text or issues inferred Backspace.
Unobserved programmatic caret/selection changes remain the same targeting
limitation as manual typing/voice. Delayed or absent application receipt cannot
cause retries, and neither prefix hints nor accepted input prove field contents.

The optional 66-DIP suggestion strip is outside the original wheel/footer. Its
owning view/cache is bounded, validates actual label fit, and hides chips when
inactive, voice-active or without a candidate. The production popup collapses
the entire strip when empty, session-hidden or disabled, restoring its ordinary
350-DIP height and shortcut footer. Resizing uses the current DPI and a bounded
work-area anchor; changing the selection alone does not allocate a new surface.
Normal keyboard dimensions and pixels with the setting disabled are unchanged. Development fixtures seed only
synthetic keyboard snapshots and do not start controller polling.

Empty optional labels bypass DirectWrite layout rather than passing a null text
pointer. A drawing/placement failure remains visible, hides the popup and blocks
that keyboard session. Closing retains the diagnostic but permits neutral
reacquisition; a deliberate new open constructs a fresh renderer. Only a
successful new draw clears the error. There is no same-session render retry or
input replay, and the accepted-key cleanup ledger is not reset.

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

Settings schema 12 adds default-on word suggestions; voice remains default-off.
All earlier supported schemas (1-11) are read without writes;
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
