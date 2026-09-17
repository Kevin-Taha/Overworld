# Building

Run all commands from the directory containing this application's `CMakeLists.txt`.
The same commands work in a standalone checkout or a nested application directory.
Build output goes under `out`; it is not source for publication.

## Prerequisites

- Windows with Visual Studio 2022 and the C++ desktop workload.
- x64 and, for cross-compilation, ARM64 MSVC tools.
- Windows SDK 10.0.26100.0 for the pinned configuration.
- CMake 3.24 or newer, Git for revision labels, and PowerShell for helper scripts.

The application is compiled with MSVC, not a whole-app Clang toolchain.
Project targets retain `/W4 /WX /permissive- /utf-8`.
Builds without a local Git repository remain usable and report an explicit
`unversioned` source fingerprint instead of inheriting an enclosing repository's
revision.

## Pointer and keyboard without speech

This route needs no speech dependency or model download:

```powershell
cmake -S . -B out\x64 -G "Visual Studio 17 2022" -A x64 `
  '-DCMAKE_SYSTEM_VERSION=10.0.26100.0' -DGAMEPAD_VOICE_WHISPER=OFF
cmake --build out\x64 --config Release
ctest --test-dir out\x64 -C Release --output-on-failure
```

Use `-A ARM64` and a separate `out\arm64` directory for ARM64.
The generated executable is `out\<build>\Release\GamepadDesktop.exe`.
Default tests do not launch the live application or microphone.

## Development-only component harness

Practice is not a shipping mode. The interactive development target and its
render-only helper must be built explicitly:

```powershell
cmake --build out\x64 --config Release --target GamepadDesktopPractice gamepad_practice_visual_tests --parallel 1 -- /nodeReuse:false
# Deliberate, separately authorized interactive demo only:
.\out\x64\Release\GamepadDesktopPractice.exe
```

Use the matching ARM64 build directory to cross-compile both targets. The
harness links shared production Pointer, Daisywheel, keyboard/cursor rendering,
app appearance, and XInput libraries, but not the voice backend or model.
It can be built with voice OFF and `BUILD_TESTING=OFF` (omit the test target).
The shipping `GamepadDesktop` target never links practice modules and rejects
`--practice` instead of starting the mapper. See the
[harness component map](PRACTICE-HARNESS.md) before interpreting demo results.

## x64 local voice

The default x64 voice route downloads a hash-pinned whisper.cpp source archive
at configure time. The application itself never downloads models or code.

```powershell
cmake -S . -B out\voice-x64 -G "Visual Studio 17 2022" -A x64 `
  '-DCMAKE_SYSTEM_VERSION=10.0.26100.0' `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_CONFIGURATION_TYPES=Release `
  -DGAMEPAD_VOICE_WHISPER=ON -DGAMEPAD_VOICE_AVX2=OFF
cmake --build out\voice-x64 --config Release
ctest --test-dir out\voice-x64 -C Release --output-on-failure
```

`GAMEPAD_VOICE_AVX2=ON` makes a distinct x64 flavor that requires
AVX2/FMA/F16C/BMI2 from process start. Do not distribute it as a portable fallback.
The build label records `voice-off`, `voice-portable`, or `voice-avx2`.

## Native ARM64 voice and reusable dependency packages

Follow [ARM64 Whisper](ARM64-WHISPER.md). The isolated dependency uses pinned
clang-cl 20.1.8; the app remains MSVC. The same package route supports x64 and
allows an app rebuild with no network access:

```powershell
$package = (Resolve-Path .\out\whisper-arm64-library\package).Path
cmake -S . -B out\arm64 -G "Visual Studio 17 2022" -A ARM64 `
  -T "v143,version=14.44.35207" '-DCMAKE_SYSTEM_VERSION=10.0.26100.0' `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_CONFIGURATION_TYPES=Release `
  -DGAMEPAD_VOICE_WHISPER=ON -DGAMEPAD_VOICE_AVX2=OFF `
  "-DGAMEPAD_WHISPER_ROOT=$package" '-DCMAKE_EXE_LINKER_FLAGS=/MAP'
cmake --build out\arm64 --config Release
```

Only use trusted package manifests: CMake package metadata is executable build
configuration, not an untrusted plugin format. The importer checks package
source/version, architecture, ABI, CRT, and per-file hashes.

## Models and prerecorded fixtures

This is an explicit developer download into ignored build output:

```powershell
cmake "-DOUTPUT_DIR=$PWD\out\voice-models" -P .\cmake\PrepareVoiceModels.cmake
```

It fetches the exact tiny.en model and a public prerecorded JFK speech fixture,
both hash-verified. Do not commit or automatically upload these files.
The model has SHA-256
`921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f`
and size 77,704,715 bytes. Production accepts only these bytes, including when a
custom model path is configured.

## Release files

Distribute the matching architecture's EXE, the exact `models\ggml-tiny.en.bin`
for voice-enabled builds, and dependency licenses/notices. Keep the model as a
separate file; do not make a ZIP or include the dependency build tree.

`PackageWhisperArm64.ps1` validates and stages a native ARM64 Release folder.
It checks the current committed source revision, PE architecture, voice flavor,
imports, and model hash, then writes file hashes to `manifest.json`.
It deliberately refuses a mismatched or uncommitted build. For an unversioned
source snapshot, build/test first; initialize and commit the approved standalone
source repository, then rebuild before using the release packager.

All builds embed separately licensed word-frequency data, even when the runtime
suggestion option is off. Include `prediction-licenses\prediction-notices.txt`
and `prediction-licenses\prediction-CC-BY-SA-4.0.txt` from the build in the
release's `licenses` directory. The ARM64 packager includes these files.
The filtered data is reproducible using `cmake\GeneratePredictionVocabulary.ps1`;
its source pin, hash, transformation and CC BY-SA 4.0 attribution are in
`third_party\prediction-notices.txt`. No runtime dictionary file is required.
The vocabulary data's ShareAlike license is separate from application code.

Include `voice-licenses\whisper.cpp-LICENSE.txt` from the build and
`third_party\voice-notices.txt` with voice-enabled distributions. These cover the
named third-party code/model, not a new license for this application's source.
LLVM, SDK, MSVC, static libraries, headers, PDBs, compiler logs, fixture audio, and
model caches are not application release assets.
`GamepadDesktopPractice.exe` and all probe/test executables are development-only
and must never be included in product release assets or added as product modes.

## Build identity

Windows file properties and Details include semantic version, local revision
when available, content fingerprint, target architecture/configuration, and speech
flavor. It is provenance, not a signature or proof of device testing. The build
does not contact a remote repository to generate it.

Use separate build directories for each architecture, compiler configuration,
and dependency flavor. Do not reuse a build tree after moving its source directory;
CMake caches absolute paths.
