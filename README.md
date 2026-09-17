# Gamepad Desktop

A native Windows utility for using an XInput controller on the desktop: pointer
movement, clicking and scrolling, a floating controller keyboard, and optional
offline push-to-talk dictation.

Desktop control activates after a controller returns to neutral and receives
deliberate input. Physical mouse or keyboard activity takes over immediately.
Automatic game switching is best-effort; manual mode switching and application
exclusions remain available. The application does not install a driver, service,
startup entry, or replacement shell.

## Get started

Use an x64 or ARM64 build matching the device. For a voice-enabled release, keep
the verified model and license files alongside the executable:

```text
GamepadDesktop.exe
models\
  ggml-tiny.en.bin
licenses\
  whisper.cpp-LICENSE.txt
  voice-notices.txt
```

Release assets are distributed as individual files, not ZIP archives. Close any
other copy before launching `GamepadDesktop.exe`. **Details** and Windows file
properties identify the version, source fingerprint, architecture, and voice
flavor. Source builds are described in [Building](docs/BUILDING.md).

### Pointer and mode controls

| Input | Action |
| --- | --- |
| Left stick | Move the pointer |
| Right stick vertically | Scroll |
| A | Primary click; hold to drag |
| X | Secondary click |
| B | Escape |
| LB | Precision movement |
| Y after neutral | Open the keyboard at the focused field |
| Menu + View | Switch between desktop mapping and native gamepad input |
| Guide / Xbox | Unchanged |

In one-handed keyboard mode, left-stick click can also open the keyboard.
Use **Ctrl+Alt+F10** or tray **Pause** for emergency pause. Resume deliberately with
Menu+View or **Desktop control**. Recovery faults are not cleared by switching modes.

The custom cursor appears only after controller activity. When an identified
Windows Start surface is active, the app requests a native-cursor fallback by
restoring only hiding it owns. This cannot make an externally hidden cursor visible.

## Floating keyboard

Focus a text field, release the controls, and press Y. The keyboard does not take
focus away from the field. Characters are typed immediately; there is no separate
composition or submit step in the live keyboard.

| Input | Action |
| --- | --- |
| Left stick | Choose one of eight clockwise character groups |
| A / B / X / Y | Choose bottom / right / left / top character |
| LB / RB | Backspace / space |
| Hold LT | Capitals |
| Hold RT | Numbers and symbols |
| Hold LT + RT | Additional symbols |
| Menu release | Enter |
| View | Close without undoing text already typed |

For **!**, hold RT and choose the top/Y slot in the `@ # & !` group.
Backspace edits existing field content, including text entered before opening
the keyboard. Live typing has no session-length draft limit.

**One-handed flicks**, selected in Settings, uses two gestures: flick toward a
group and center, then flick toward a character and center. Pending choices are
highlighted before commitment. The upper-left action group offers Space (down),
Backspace (right), page cycle (left), and Enter (up). All printable ASCII remains
reachable across the pages.

Focus changes, physical takeover, controller loss, mode changes, and unsafe
contexts stop typing. Pending edits never move to a new target or replay
automatically. A browser or custom control may change its internal field without
changing native HWND identity, so exact targeting cannot be guaranteed for all apps.

## Offline voice

In **Settings > Typing**, enable **Local voice input** in a voice-enabled build.
The default model path is `models\ggml-tiny.en.bin` beside the EXE. An override must
contain the same verified tiny.en model; other bytes are rejected before parsing.

While the keyboard is open, hold **right-stick click**, wait for **Listening**,
speak, then release to transcribe and insert. **View cancels**. Loading is not
recording; the UI distinguishes loading, listening, transcribing, and errors.
Capture is limited to 25 seconds and requires continuously renewed authorization.

Recognition runs locally with pinned whisper.cpp. There is no runtime model
download, audio upload, silent cloud fallback, transcript persistence, automatic
Enter, or speech-command execution. Recording uses the Windows default microphone
and its existing permission settings.

| Build flavor | Requirement |
| --- | --- |
| `voice-off` | No recognizer backend; pointer and keyboard remain available |
| `voice-portable` | Baseline CPU Whisper, including native ARM64 static libraries |
| `voice-avx2` | x64 CPU supporting AVX2, FMA, F16C, and BMI2 from process start |

See [ARM64 Whisper](docs/ARM64-WHISPER.md) for the isolated dependency build and
[Privacy](docs/PRIVACY.md) for recording and diagnostic boundaries.

## Settings and optional features

Settings has **General**, **Pointer**, **Typing**, and **Advanced** tabs, with
system app light/dark colors, high contrast, and keyboard navigation.

- **Smooth** is the default pointer profile: 1100 pixels/s maximum, LB precision
  25%, scroll 960 wheel units/s, and left/right stop-start deadzones of 20/24%
  and 23/27%. **Speed bands** provides adjustable low/mid/high rates; equal adjacent
  rates combine speeds. Existing settings are preserved.
- **Automatic game switching** uses documented presentation state and foreground
  fullscreen pointer confinement. It can miss games or classify non-games.
  Menu+View can request desktop control for the current app activation; this does
  not bypass exclusions, physical conflicts, security, or recovery checks.
- **Application exclusions** are exact executable basenames, one per line.
  They are not a generic game allowlist or permission bypass.
- **Keyboard in native mode** is a default-off experiment. With a text field
  focused, hold View+Y for half a second and release both before typing. It permits
  text only while pointer mapping stays off; controller input can still reach the game.
- **Filter native controller navigation** is a default-off experiment for competing
  injected `VK_GAMEPAD` events. It does not suppress all gamepad APIs, physical keys,
  ordinary navigation keys, or touch-keyboard behavior. The first hold may pass
  through, identical sources cannot always be distinguished, and D-pad desktop
  mapping is not provided.

Settings live at `%LOCALAPPDATA%\GamepadDesktop\settings.ini`. Loading older
supported schemas never rewrites the file; **Save** validates and detects external
changes. Defaults affect the editor until saved. Back up settings before returning
to an older executable, which may not understand the current schema.

## Development

- [Building and release files](docs/BUILDING.md)
- [Testing and interactive-test safety](docs/TESTING.md)
- [Architecture and invariants](docs/ARCHITECTURE.md)
- [Troubleshooting and known limits](docs/TROUBLESHOOTING.md)
- [Contributing](CONTRIBUTING.md) and [agent instructions](AGENTS.md)
- [Scope and roadmap](docs/PLAN.md)

No application license is currently granted by this repository. Third-party
notices and their license requirements are retained in
[`third_party/voice-notices.txt`](third_party/voice-notices.txt); they apply to the
named dependencies, not to the application as a whole.
