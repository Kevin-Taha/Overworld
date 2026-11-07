# Overworld

A native Windows utility for controlling the desktop with a game controller:
move the pointer, click and scroll, type with a floating controller keyboard,
and optionally dictate short text offline.

**Windows only | x64 and ARM64 | early development.** Overworld is not a
replacement shell, universal game detector, or accessibility guarantee.
It installs no driver, service, startup entry, or external remapper.

## Features

- **Controller-to-desktop handoff:** XInput and mapped DirectInput/HID controllers,
  neutral-before-activation, immediate physical mouse/keyboard takeover, and an
  emergency pause.
- **Pointer and typing:** adjustable pointer profiles, precision movement,
  scrolling, a non-focus-stealing floating keyboard, and optional one-handed flicks.
- **Local word suggestions:** an embedded English frequency list, enabled by
  default with a Settings opt-out. No field-text reads, learning, or history.
- **Optional offline voice:** opt-in, push-to-talk Whisper tiny.en recognition.
  No runtime downloads, cloud fallback, transcript logging, or automatic Enter.
- **Native settings:** keyboard navigation, system light/dark appearance,
  high contrast, application exclusions, and explicit recovery feedback.

## Get started

Build from source using the [quick build below](#build-from-source), or use a
matching architecture/flavor from [Releases](https://github.com/Kevin-Taha/overworld/releases)
when one is available. Keep the supplied controller database and licenses
alongside the executable; voice-enabled builds also require the verified model.
Release files are distributed individually, not as ZIP archives.
See the [file layout and full usage guide](docs/USAGE.md#get-started).

Close any existing Overworld or legacy Gamepad Desktop instance before launching.
Release all controller inputs, then move a stick deliberately to begin.
Left stick moves, A clicks, X right-clicks, right stick scrolls, and Y opens the
keyboard at a focused field. **Ctrl+Alt+F10** or tray **Pause** stops desktop
control; Menu+View switches between desktop control and native gamepad input.

**Important limits:** automatic game switching is best-effort. Unknown controller
layouts need an explicit mapping. Browser/custom password fields cannot always
be recognized, and applications can change their internal text field without
changing native window identity. Check the target before typing or accepting a
suggestion; disable suggestions for sensitive use. Never rely on an overlay or
successful input transport as proof that an application received an action.

The [usage guide](docs/USAGE.md) documents all controller mappings, one-handed
typing, voice setup, Settings, and default-off experiments. Start with
[Troubleshooting](docs/TROUBLESHOOTING.md) for recovery and known OS/app limits;
read [Privacy](docs/PRIVACY.md) before sharing diagnostics.

## Build from source

Use Windows, Visual Studio 2022 with the C++ desktop workload and a Windows SDK,
CMake 3.24+, Git, and PowerShell. Run from the application root:

```powershell
git clone https://github.com/Kevin-Taha/overworld.git
Set-Location .\overworld
cmake -S . -B out\x64 -G "Visual Studio 17 2022" -A x64 -DOVERWORLD_VOICE_WHISPER=OFF
if ($LASTEXITCODE) { throw 'Configure failed.' }
cmake --build out\x64 --config Release --parallel 1 -- /nodeReuse:false
if ($LASTEXITCODE) { throw 'Build failed.' }
ctest --test-dir out\x64 -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Deterministic tests failed.' }
```

This builds `out\x64\Release\Overworld.exe` without speech dependencies or model
weights. Configure fetches the hash-pinned public controller mapping database and
license; [Building](docs/BUILDING.md) documents offline inputs, ARM64 targets,
voice flavors, pinned release tools, and loose-file packaging.
Default tests do not launch the live mapper, read controllers, open the
microphone, install the optional hook, or inject input.

The separately built [Practice component harness](docs/PRACTICE-HARNESS.md)
uses shared production models/renderers with an owned scene. It is a development
tool, not a product mode or release asset; `Overworld.exe --practice` is invalid.

## Development and contributions

Read [Contributing](CONTRIBUTING.md), [Architecture](docs/ARCHITECTURE.md),
[Testing](docs/TESTING.md), and [agent instructions](AGENTS.md) before changing
behavior. The [roadmap](docs/PLAN.md) records scope and retired experiment
decisions, not an automatically scheduled work queue.

Use [Issues](https://github.com/Kevin-Taha/overworld/issues) for redacted bug reports
and focused proposals. For sensitive vulnerabilities, follow
[Security reporting](SECURITY.md) instead of opening a public issue.
Maintainers should review [Publication readiness](docs/PUBLISHING.md) before
changing visibility or distributing a release.

## License and third-party material

Overworld's original code and documentation are licensed under the [MIT License](LICENSE).
Separately identified third-party material retains its own terms:

| Material | License and attribution |
| --- | --- |
| Embedded word-frequency vocabulary, including its filtered selection/order | [CC BY-SA 4.0](third_party/prediction-CC-BY-SA-4.0.txt), with [source, modifications, and attribution](third_party/prediction-notices.txt) |
| SDL-format controller mapping database | zlib license, fetched and hash-verified with the pinned database; [details](docs/CONTROLLERS.md) |
| Optional whisper.cpp/ggml and Whisper tiny.en weights | MIT; [voice notices](third_party/voice-notices.txt) and the fetched dependency license |

Include `LICENSE` and the required third-party notices/licenses with
distributions, including voice-off builds. MIT does not replace the vocabulary's
attribution/ShareAlike requirements. The vocabulary comes from subtitles and can
contain names, slang, and offensive language; it is not child-safe.
