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
