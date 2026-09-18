# Practice development harness

`GamepadDesktopPractice.exe` is a **development/demo tool, not a product
feature, first-run experience, or release asset**. It is excluded from the
default build and has no entry point in `GamepadDesktop.exe`. The old shipping
`--practice` flag fails closed as invalid input; it cannot activate the mapper.

## Build and deliberate launch

From an existing x64 or ARM64 build configured as in [Building](BUILDING.md):

```powershell
cmake --build out\x64 --config Release --target GamepadDesktopPractice --parallel 1 -- /nodeReuse:false
.\out\x64\Release\GamepadDesktopPractice.exe
```

Use `out\arm64` for an ARM64 build. `--help` explains the development boundary;
other flags are rejected before creating the reader. Neither models nor Whisper
are linked or needed by this target, even when the separate shipping app is
configured with voice enabled. A dedicated harness-only build can use
`-DGAMEPAD_VOICE_WHISPER=OFF -DBUILD_TESTING=OFF` and the same explicit target.
Do not put this executable or its probes in product release directories.

Controller observation requires deliberate interactive-test authorization.
Close any running mapper or harness first: both executables acquire the same
`MapperInstanceGuard`, including compatibility with older mapper releases.
The harness never activates another window. Select its clearly labeled
**Practice (development only) - no desktop input** window and center/release
the controller.

## Actual shared components

| Shared library / source | Exercised in this harness | Not established by this exercise |
| --- | --- | --- |
| `gamepad_core` / `Pointer.cpp` | Production default motion curve, stick hysteresis/deadzones, integration cap, LB precision, wheel deltas; scene coordinates preserve physical-pixel speed | `Live` output ledger, Windows pointer movement, desktop bounds, or saved pointer preferences |
| `gamepad_keyboard` / `Keyboard.cpp` | The actual `Daisywheel` selection, pages, repeat/edit rules, face-button and one-handed gestures; bounded preview text | Live unbounded edit delivery, external-field targeting, partially accepted input recovery |
| `gamepad_keyboard` / `WordPrediction.cpp`, `KeyboardDelivery.cpp` | Explicit deterministic harness tests use `PredictionPractice`, a bounded fake field, with the same compiled default-on predictor and 10,000-word dictionary | The interactive five-scene harness does not yet present a suggestion-selection scenario; no native target/privacy or external delivery is simulated successfully |
| `gamepad_keyboard_visual` / `KeyboardVisual.cpp` | The same compiled renderer, glyph layout, theme, alpha, cache, and keyboard pixels used by the production popup | Nonactivating popup placement over other apps, compositor visibility in other app surfaces |
| `gamepad_cursor_pixels` / `RingPixels.cpp` | Exact production default cursor artwork and geometry, with shared size/style defaults also used by `Settings` | Native cursor visibility ownership, layered cursor window, surface fallback |
| `gamepad_app_ui` / `AppUi.cpp` | Production palette/theme reads and native appearance support | A duplicate Settings editor; the training cards/buttons are owned test fixtures |
| `gamepad_xinput` / `XInputReader.cpp` | The same one-slot decoder and connection epochs, behind the harness's scoped reader | Production poller timing; the normal four-slot `read()` remains unchanged |
| `InputClassification.h` / `Live.cpp` | Existing reserved controller/navigation key classification for own-window takeover | A global hook, native navigation filter, or physical-input observer |
| `InstanceGuard.h` | The same mapper-instance exclusion | Native output permission or a bypass of an already-running mapper |

`Practice.cpp` is the local scene sink, progress, and safe input adapter.
`PracticeWindow` and `PracticeReader` own the development-only UI/read lifecycle.
They are not production delivery models. No `Desktop`, `NativeVoice`,
`NativeCursorVisibility`, navigation hook, UIA, clipboard, or settings persistence
boundary is constructed. The microphone is never opened: Help only explains the
live voice controls and never reports a recording/transcription success.
New features are not automatically covered merely because they link to the app.
Update this table and the relevant scenarios/contracts when adding one.

Prediction's bounded fake scenario opens each real keyboard selection mode,
types the synthetic `he` fragment, compares candidates with the shared
`WordPrediction` model/dictionary, and clears on handoff. The separate explicit
`gamepad_keyboard_visual_tests` renders that fixture through the same
`gamepad_keyboard_visual` library, covering candidate labels, theme/DPI, alpha
and idle reuse. It is not another product mode or copied predictor/renderer.
Product prediction-preview/demo flags are rejected. The interactive harness's
local click lesson is not evidence for the opt-in production `--release-tap`
dispatcher; the latter has separate fake `gamepad_release_tap_tests`.

The live Host's desktop/native keyboard eligibility orchestration is not a
practice scene. `gamepad_delivery_tests` exercises its shared
`KeyboardRouting.h` helper with a bounded stateful fake route and the real
`KeyboardDelivery` model. Neither that fixture nor practice establishes shell
overlay ordering, native navigation exclusivity, or touch-keyboard behavior.

## Try a component change

Five scenes exercise aiming, A/X clicks, hold-and-drag, scrolling, and local
keyboard selection. The default production cursor stays inside the harness.
Point and tap A on the native lesson/Retry/Reset/Help/Exit controls. Menu advances
the fixture; View closes keyboard/help first, or exits. Y opens local typing.
These Menu/View fixture actions are not an imitation of the production mode
switch. Close the keyboard before pointing at other fixture controls.

The local keyboard uses its real character groups: left stick up plus A types
`a`. The one-handed button selects the actual two-stage flick model. Local
group/face buttons exercise that same model; its action group's page cycle
retains all four pages across later local actions and idle gaps.

Native Tab navigation and mouse controls remain available. With the scene
focused: arrows move, Enter clicks, Space holds a drag, X right-clicks, Page
Up/Down scroll, and Y opens typing. F1 opens Help; Escape dismisses keyboard,
help, or options first, otherwise exits. Leaving the scene/control mode cancels
a local hold without dropping. Matching Space release is handled even after
Tab changes the child focus.

Focus loss, physical takeover, reconnection, ambiguity, stale samples, and
input gaps cancel gestures and require new neutral. Timestamp fences reject
unread neutrals from before a transition. The reader checks stop, original
50 ms batch lease, scope generation, and actual foreground/minimized state
before **each slot and publication**. A renewed lease cannot revive an old batch.
Shutdown waits at most 250 ms and reports failure instead of restarting.
XInput offers no cancellation for an in-flight call: retained worker-owned state
survives a timeout, terminal stop forbids later slots/publication, and the
harness process exits. This is not a hard API-cancellation guarantee.

## Contracts and evidence

`cmake/PracticeContracts.cmake` checks actual CMake target dependencies:
the shipping target cannot link practice modules; the harness is excluded from
the default build; both use the shared production libraries; each shared `.cpp`
has one compilation owner. There is no harness-specific copy of the keyboard,
cursor artwork, or app theme implementation.

Default tests check separate fail-closed parsers and build/source isolation.
Default `ALL_BUILD`, even with `BUILD_TESTING=ON`, has no practice implementation
dependency: development libraries and their test executables are all explicit.
Configure-time contracts also reject transitive development dependencies from
any non-excluded target.

Build and run the deterministic development fixtures separately:

```powershell
cmake --build out\x64 --config Release --target gamepad_practice_tests gamepad_practice_reader_tests --parallel 1 -- /nodeReuse:false
.\out\x64\Release\gamepad_practice_tests.exe
.\out\x64\Release\gamepad_practice_reader_tests.exe
```

They cover exact Pointer response equivalence, production-layout local typing,
lifecycle/input regressions, and fake delayed reader shutdown. They never query
a controller, open a microphone, or emit external input.

The explicit `gamepad_practice_visual_tests --render-only <owned-directory>`
target links the **same `gamepad_practice_view` library** as the harness and no
XInput backend. Its borrowed keyboard/cursor buffers are compared byte-for-byte
with the production renderers at light/dark/high-contrast 96/144/192 DPI.
It also covers text geometry, alpha, idle caching, native control bounds/tab
order, owned hold messages, exact paint-handler RGB output, and bounded
lifetimes. See [Testing](TESTING.md) for commands and authorization requirements.
Neither these checks nor an interactive demo validates external-app delivery,
microphone operation, screen-reader behavior, or ARM hardware execution.
