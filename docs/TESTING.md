# Testing

Run commands from the application root. Build prerequisites and speech flavors are
in [Building](BUILDING.md).

## Default deterministic tests

```powershell
cmake --build out\x64 --config Release
ctest --test-dir out\x64 -C Release --output-on-failure
```

Core, pointer/Live, cursor ownership/pixels, settings, keyboard, delivery, voice,
model validation, navigation-filter ledger, self-caption transactions, and build
identity tests run without live input injection, microphone capture, or installing
the native hook. Settings tests write only to their owned test-scratch directory.
ARM64 tests can be built by cross-compilation, but must be executed on an
appropriate ARM64 environment.

For a focused change:

```powershell
ctest --test-dir out\x64 -C Release -R '^gamepad_(keyboard|delivery)_tests$' --output-on-failure
```

Retain regressions for accepted-down/up accounting, cancellation, stale samples,
focus excursions, same-HWND field changes, expired capture leases, invalid model
bytes, strict migration, and ambiguous injected-source handling.

Prediction regressions are part of `gamepad_delivery_tests`, not a native-input
test. They cover the authored vocabulary, case/boundary rules, bounded prefixes
and choices, unknown Backspace, partial acceptance, stale generations/deadlines,
target/context/physical/device changes, queue limits, both controller modes,
mixed D-pad/action gestures, voice transitions, and the in-memory practice sink.
Native boundary prediction stays default-off and never receives prefix hints.

## Render-only checks

These targets are opt-in and excluded from default CTest because they create
native windows. Run only in an authorized test session:

```powershell
cmake --build out\x64 --config Release --target gamepad_keyboard_visual_tests gamepad_app_ui_tests
.\out\x64\Release\gamepad_keyboard_visual_tests.exe .\out\visual-frames
.\out\x64\Release\gamepad_app_ui_tests.exe .\out\app-frames
```

They use deterministic theme overrides rather than changing Windows preferences.
Keyboard tests verify actual DirectWrite ink bounds, alpha/premultiplication,
contrast, transparent space, idle equivalence, and resource reuse. App tests check
native control geometry, tab order, DPI layout, and capability feedback.
Rendered artifacts are local review evidence, not repository content.

The keyboard visual suite also exports synthetic `prediction-*` frames from the
actual no-output practice model. It checks light/dark and both high-contrast
palettes at 100/150/200%, actual DirectWrite label fit, 4.5:1 suggestion ink,
unchanged wheel pixels, premultiplied alpha, owning cached strings, idle reuse,
and hidden chips during inactivity/voice/no-prefix states. No real app text is
used. `--prediction-demo` is a separate static no-output UI demo; it starts no
controller poller, mapper, microphone or hook. Preview-only success does not
validate external-app prediction or universal password detection.

Do not use `PrintWindow` success alone as visibility evidence for layered windows.
Inspect exact renderer frames at native and scaled DPI over light/dark backdrops.
Actual assistive-technology and multi-monitor behavior still needs device testing.

## Owned-message probes

The ordinary `gamepad_self_caption_tests` invocation is pure. Its
`--owned-messages` option creates owned windows and sends only window messages,
not global mouse input. The `--tracker-baseline` option intentionally demonstrates
a native non-client tracking stall and **must only run under an external bounded
process watchdog**. It is not a routine smoke command.

`gamepad_ring_smoke` is another explicit native-window probe. Review its source and
invocation guard before running; do not assume an executable named "test" is safe
to launch unattended.

## Prerecorded speech

With a voice-enabled build and explicitly obtained verified assets:

```powershell
cmake --build out\voice-x64 --config Release --target gamepad_voice_fixture
.\out\voice-x64\Release\gamepad_voice_fixture.exe `
  .\out\voice-models\ggml-tiny.en.bin .\out\voice-models\jfk.wav
```

This exercises actual local inference, silence rejection, and cancellation without
opening the microphone. The public fixture is not a substitute for testing the
default recording device, microphone permission, language/accent, or live latency.
Do not substitute personal recordings in automated runs or retain transcripts.

For isolated dependency imports and architecture verification:

```powershell
.\tests\WhisperImportTests.ps1 -X64Package .\out\whisper-x64-library\package
.\tests\WhisperBuildTests.ps1 -DependencyBuild .\out\whisper-arm64-library `
  -AppBuild .\out\arm64 -Architecture ARM64 -LlvmRoot 'C:\Tools\LLVM-20.1.8'
```

Import tests create and damage their own package copy; never point their scratch
root at a valuable directory. Build audits inspect architecture, CRT, imports,
and symbols; they do not run an ARM64 executable.

## Interactive checks: explicit authorization required

Normal application launch activates automatic controller mapping.
`--inspect` disables external input and the optional filter, but still creates a
real app window and reads settings/observers. Keyboard previews are no-output
design modes. Do not start a second copy against someone else's running instance.

`gamepad_text_smoke --allow-owned-text-smoke` (optionally `--one-handed`) is an
actual native input test. It must be separately approved, use only its disposable
owned fields, and retain its PID/HWND, foreground, physical-input, deadline, and
single-instance guards. Never bypass a blocked or locked-desktop result.

Manual verification should distinguish:

- Mouse versus controller clicks, including own caption buttons and shell surfaces.
- Ordinary keyboard edits versus microphone dictation.
- New output versus already-accepted cleanup obligations.
- Application acceptance versus `SendInput` transport acceptance.
- Build/fixture results versus actual user-device behavior.

Stop on unexpected output with the emergency pause. Do not replay an uncertain
edit or clear an ownership fault merely to make a test pass.
