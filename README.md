# Gamepad Desktop

A native Windows utility for using a supported controller on the desktop: pointer
movement, clicking and scrolling, a floating controller keyboard, and optional
offline push-to-talk dictation.

Desktop control activates after a controller returns to neutral and receives
deliberate input. Physical mouse or keyboard activity takes over immediately.
Automatic game switching is best-effort; manual mode switching and application
exclusions remain available. The application does not install a driver, service,
startup entry, or replacement shell.

## Controller compatibility

Xbox-compatible **XInput** controllers and Windows **DirectInput/HID game
controllers** use the same pointer and keyboard controls. Known layouts use the
bundled SDL-format community mapping database; unknown layouts require an explicit
mapping rather than guessed click/type buttons. No virtual Xbox controller,
driver installation, or external remapper is required.

Use Automatic controller preference, release all controls, then move a stick
deliberately. Up to four controllers share app slots; XInput retains its slot
priority. Reconnection or a slot-source change requires fresh neutral input.
Face-button actions follow physical Xbox-style positions (bottom/right/left/top),
not necessarily the lettering printed on a controller.

Keep the `controllers` folder alongside the executable. To add or override a
layout, place SDL-format Windows mapping lines in
`controllers\user-mappings.txt` and restart the app. See
[controller mappings](docs/CONTROLLERS.md) for instructions and limitations.

## Get started

Use an x64 or ARM64 build matching the device. For a voice-enabled release, keep
the verified model and license files alongside the executable:

```text
GamepadDesktop.exe
models\
  ggml-tiny.en.bin
controllers\
  gamecontrollerdb.txt
licenses\
  SDL_GameControllerDB-LICENSE.txt
  whisper.cpp-LICENSE.txt
  voice-notices.txt
  prediction-notices.txt
  prediction-CC-BY-SA-4.0.txt
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

**Optional release-aware taps:** start `GamepadDesktop.exe --release-tap` after
closing any running instance. The custom cursor and normal Start fallback stay
unchanged. A stationary short A press clicks on release; holding for 250 ms or
moving while held starts a drag. Target/routing changes cancel an uncommitted
tap, and accepted button releases retain normal cleanup ownership.
Normal launch still clicks on A-down. This experiment does not establish a fix
for initial Start/Phone Link interaction or late native-navigation bridge events.

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

When typing into Windows Start or Search, the keyboard tries to fit beside the
shell window instead of relying on drawing above it. It keeps its current
position when clear and otherwise chooses the nearest available full-size spot
on that monitor. If no spot fits or shell geometry cannot be verified, typing
pauses; **View** closes the keyboard so it can be reopened deliberately.
This avoids known shell-window bounds, not every possible OS overlay or compositor
effect. It never dismisses Start or takes focus away from the text field.

## Local word suggestions

Word suggestions are **on by default**. Open the keyboard at an otherwise
eligible target and type two letters to see up to three frequency-ranked choices.
No leading Space or per-field enable gesture is required. Turn off **Settings >
Typing > Word suggestions** and Save to opt out. **D-pad Down** hides suggestions
for the current keyboard session only; reopening uses the saved preference.
Neither action erases text already typed. Hidden, disabled or empty suggestions
take no extra space: the keyboard returns to its ordinary size and shortcuts.

**D-pad Left/Right** chooses; **D-pad Up** appends only the remaining suffix once.
Return to neutral between presses and finish an in-progress one-handed flick
before choosing. No automatic Space, Enter, replacement or inferred Backspace is
performed. Existing face-button, one-handed, LB/RB/Menu/View mappings are unchanged.
A selected completion is an explicit request to type its literal suffix, like
ordinary manual text or dictation. It uses the same original-target validation,
bounded output queue and accepted-key release ledger, with one fixed one-second
deadline. Unsent characters are cancelled on known interruptions; partial output
is never replayed. No caret or application-receipt synchronization is attempted.

**Coverage follows existing keyboard typing**, including browser, Notepad,
RichEdit, single-line and custom targets; there is no application/class allowlist,
field-length limit or Edit-specific message requirement. The existing mapper
security, focus, session/integrity and exclusion checks still apply.
Recognized native Edit/RichEdit password styles and failed metadata queries
quietly suppress suggestions. Unknown/custom field privacy cannot be verified:
browser password fields can be unknown. This is **not universal password detection**.
Ordinary typing is unchanged when suggestions are off or suppressed.

Only this session's transport-completed keyboard edits feed a maximum 16-letter
ephemeral word fragment, not field contents. Space/punctuation reset it; observed
physical keyboard/mouse navigation, takeover, target/context/device/mode changes,
unknown Backspace, close and voice transitions clear or invalidate hints.
After a handoff or voice transition, only newly accepted own letters can form a
new fragment. Programmatic caret or
selection changes and internal field changes invisible to the existing target
checks remain limitations, just as for manual text/dictation. The app may discard
input or apply it at its current caret/selection; transport acceptance is not
proof of receipt or atomic insertion. Check your target before accepting.

The embedded 10,000-word English frequency list is derived from the pinned
HermitDave/FrequencyWords OpenSubtitles2018 data under **CC BY-SA 4.0**; see
[`third_party/prediction-notices.txt`](third_party/prediction-notices.txt).
It can contain names, slang and offensive language; it is not child-safe.
No ML, cloud, OS dictionary, personal word list, clipboard, field-text read,
learning or history is used. Full data attribution and license must accompany
the EXE; the data license does not grant an application-code license.

Development tests use a bounded in-memory prediction fixture and the same
compiled predictor, dictionary and keyboard renderer; see the
[harness coverage contract](docs/PRACTICE-HARNESS.md). Prediction demo/preview
launch modes are not part of the shipping application.

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
changes and writes schema 12, including the Word suggestions preference.
Defaults affect the editor until saved. Back up settings before returning
to an older executable, which may not understand the current schema.

## Development

Starting on another machine or in a fresh agent session? Read the
[agent instructions](AGENTS.md), inspect local build inputs with
[Building](docs/BUILDING.md), and recreate the default deterministic baseline.
The [roadmap](docs/PLAN.md) records declined and parked experiments; they are not
an automatically scheduled work queue.

The [Practice component harness](docs/PRACTICE-HARNESS.md) is a separately built
development/demo tool, **not a product feature or release asset**. It links the
same production models and rendering libraries, with an owned scene instead of
external input delivery. `GamepadDesktop.exe --practice` is not supported; it
exits as an invalid command rather than starting desktop control.

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
