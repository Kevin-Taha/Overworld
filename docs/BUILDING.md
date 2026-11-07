# Building

Run all commands from the directory containing this application's `CMakeLists.txt`.
The same commands work in a standalone checkout or a nested application directory.
Build output normally goes under `out`; it is not source for publication.
An absolute build directory on another drive is also supported. Keep separate
directories for each architecture/flavor, and set process-local `TEMP` and `TMP`
to that drive when the system drive has limited space.

## Prerequisites

The application target/executable is `Overworld`, development harness is
`OverworldPractice`, libraries and tests use `overworld_*`, and CMake options use
`OVERWORLD_*`. Configure a fresh build directory after upgrading from the former
name; legacy `GAMEPAD_*` build options fail explicitly rather than silently
disabling requested features. Use `PackageOverworldArm64.ps1` to stage ARM64 files.

- Windows with Visual Studio 2022 and the C++ desktop workload. The checked
  release configuration uses the v143 14.44.35207 toolset.
- MSVC x64 tools and, for the ARM64 target, the ARM64 C++ tools.
- Windows SDK; the pinned release configuration uses 10.0.26100.0.
- CMake 3.24 or newer, Git for revision labels, and PowerShell for helper scripts.

The application is compiled with MSVC, not a whole-app Clang toolchain.
Project targets retain `/W4 /WX /permissive- /utf-8`.
For ordinary voice-off development, omit the explicit toolset/SDK pins to use
the installed VS 2022 tools and SDK, or specify an installed SDK with
`-DCMAKE_SYSTEM_VERSION=<version>`. The pinned voice-release/package recipes
below remain separate; do not claim an ordinary build reproduced those inputs.
Builds without a local Git repository remain usable and report an explicit
`unversioned` source fingerprint instead of inheriting an enclosing repository's
revision.

The application builds with native MSVC tools on x64 and ARM64 Windows.
`-A ARM64` selects the **target**, not the host tools. Inspect the host architecture
and installed tools first. The dependency helper currently assumes x64-host tools;
for native ARM64 LLVM use the explicit recipe in [ARM64 Whisper](ARM64-WHISPER.md).
Compilation and deterministic tests do not establish live device compatibility.

## Fresh clone and relocated static packages

The repository contains source, tests and notices, not compilers, static dependency
packages, model weights or generated binaries. Clone using the new machine's
normal Git setup; do not copy credentials or an old agent directory:

```powershell
git clone https://github.com/Kevin-Taha/overworld.git
Set-Location .\overworld
git status --short
git log -1 --oneline
cmake --version
```

Read [the roadmap](PLAN.md) before choosing new work. For voice-enabled
development, importing a complete trusted package avoids rebuilding dependencies.
Place a copied package at any location you choose, for example
`out\dependencies\whisper-x64` or `out\dependencies\whisper-arm64`. Each must
contain `manifest.cmake`, the entire `include` and `lib` directories, and
`whisper.cpp-LICENSE.txt`. No LLVM installation or old dependency build tree is
needed merely to import it.

Run from the new source root in a new build directory:

```powershell
$architecture = 'x64' # Or 'ARM64' for that target, not a claim about the host.
$suffix = $architecture.ToLowerInvariant()
$package = (Resolve-Path ".\out\dependencies\whisper-$suffix").Path
$build = "out\voice-$suffix"
cmake -S . -B $build -G "Visual Studio 17 2022" -A $architecture `
  -T "v143,version=14.44.35207" '-DCMAKE_SYSTEM_VERSION=10.0.26100.0' `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_CONFIGURATION_TYPES=Release `
  -DOVERWORLD_VOICE_WHISPER=ON -DOVERWORLD_VOICE_AVX2=OFF `
  "-DOVERWORLD_WHISPER_ROOT=$package" '-DCMAKE_EXE_LINKER_FLAGS=/MAP'
if ($LASTEXITCODE) { throw 'Application configure failed.' }
cmake --build $build --config Release --parallel 1 -- /nodeReuse:false
if ($LASTEXITCODE) { throw 'Application build failed.' }
# Execute only on a host/runtime appropriate for the selected target:
ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Deterministic tests failed.' }
```

The package manifest uses relative paths; the importer resolves them under the
new `OVERWORLD_WHISPER_ROOT`. It checks the declared target architecture, source
and LLVM pins, compatible MSVC ABI floor, all listed file hashes, Release-only
`/MT`, and AVX2 OFF. A compiler older than the manifest ABI is rejected.
The manifest is executable CMake, so its own provenance must be trusted; file
hash checks do not make an untrusted manifest safe. This import validation is
not a fresh per-object architecture or compiler-command audit.
Version-1 dependency manifests deliberately retain their `GAMEPAD_WHISPER_*`
schema keys. Existing trusted packages remain usable without rewriting hashes or
rebuilding Whisper. These private package fields are not build/cache option names.

Successful package import is configure-only evidence, not a build or runtime test.
Reconfigure from scratch after moving source:
do not reuse another machine's CMake cache, absolute build paths or generated
project files. Use separate directories for each target and flavor.

Without a copied package, build one from pinned public source using
[the isolated dependency instructions](ARM64-WHISPER.md#build-the-dependency).
Explicit model preparation is [below](#models-and-prerecorded-fixtures).
Complete both before offline work is expected.

## Pointer and keyboard without speech

Controller support uses native XInput/DirectInput plus hash-pinned
[SDL-format mapping data](CONTROLLERS.md), not the SDL runtime. Configure downloads
the public database/license once into its build directory. For offline builds,
set `OVERWORLD_CONTROLLER_DATABASE_ROOT` to a directory containing the exact pinned
`gamecontrollerdb.txt` and `LICENSE`; missing or mismatched bytes fail explicitly.
Both app targets copy the database to `controllers` beside their EXE.
No runtime downloads or virtual-controller drivers are used.

This route needs no speech dependency or model download:

```powershell
cmake -S . -B out\x64 -G "Visual Studio 17 2022" -A x64 `
  -DOVERWORLD_VOICE_WHISPER=OFF
if ($LASTEXITCODE) { throw 'Application configure failed.' }
cmake --build out\x64 --config Release --parallel 1 -- /nodeReuse:false
if ($LASTEXITCODE) { throw 'Application build failed.' }
ctest --test-dir out\x64 -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Deterministic tests failed.' }
```

Use `-A ARM64` and a separate `out\arm64` directory for ARM64.
The generated executable is `out\<build>\Release\Overworld.exe`.
Default tests do not launch the live application or microphone.

## Development-only component harness

Practice is not a shipping mode. The interactive development target and its
render-only helper must be built explicitly:

```powershell
cmake --build out\x64 --config Release --target OverworldPractice overworld_practice_visual_tests --parallel 1 -- /nodeReuse:false
# Deliberate, separately authorized interactive demo only:
.\out\x64\Release\OverworldPractice.exe
```

Use the matching ARM64 build directory to cross-compile both targets. The
harness links shared production Pointer, Daisywheel, keyboard/cursor rendering,
app appearance, and XInput libraries, but not the voice backend or model.
It can be built with voice OFF and `BUILD_TESTING=OFF` (omit the test target).
The shipping `Overworld` target never links practice modules and rejects
`--practice` instead of starting the mapper. See the
[harness component map](PRACTICE-HARNESS.md) before interpreting demo results.

## Optional controller view

The normal `Overworld` target includes a default-off controller visualization.
No extra executable, build option, dependency package or settings migration is
required. Normal launch does not construct its worker, window, timer or renderer.
Only explicit `--controller-view` launches allocate those resources.

```powershell
cmake --build out\arm64 --config Release --target Overworld --parallel 1 -- /nodeReuse:false
# Deliberate interactive use only; close any running mapper/Practice first:
.\out\arm64\Release\Overworld.exe --controller-view
# If release-aware clicks are also wanted, request them explicitly:
.\out\arm64\Release\Overworld.exe --controller-view --release-tap
```

Keep the same controller data, optional verified voice model, and licenses beside
the executable as usual. The optional view preserves normal settings and
single-instance protection; it cannot attach to an already-running instance.
Both flag orders are accepted with `--release-tap`; duplicates and combinations
with inspection, keyboard previews or cursor recovery are rejected before
starting input. This is a live app, **not** a no-output preview. No build/test
step launches it automatically.

The borderless, movable, resizable, nonactivating controller window is intended
for recordings. Its compact, rounded controller artwork is intentionally label-free:
thumbstick movement, trigger fill and restrained press highlights show activity
without diagnostic headings or a legend. The rectangular background is fully
transparent; the controller body is about 90% opaque while input indicators
remain opaque. High contrast keeps the controller surfaces opaque. The native
title bar is not drawn, but the accessible window name retains status information.
Its independent rendering thread
receives the exact batch already consumed by the Host; it never opens another
controller reader. It follows the keyboard's pinned slot while typing and the
pointer's selected slot otherwise.
When no slot is selected it shows a sole connected controller as observation
only, or reports ambiguity in its accessible name if several are connected.
Buttons/sticks/triggers show mapped raw values, not deadzone-filtered pointer
motion or proof of delivered input. Guide is not exposed by the reader and is
not fabricated.

Drag a blank painted part of the controller to move it. Drag its visible outer
edges/grip corners to resize; transparent window margins are not resize handles.
Right-click the controller to close just the companion, leaving the mapper
running and releasing view resources. Restart with the flag to reopen it. The
flag is not persisted in Settings. There is no popup menu or permanent
close button. Moving it with a physical mouse still invokes normal mouse takeover.
Old/unavailable samples clear pressed controls rather than appearing stuck.
Controller availability and routing details are not drawn over the controller.
There is no recording, event history, network transport, settings write, or
change to output authorization from this view. Transparent-window capture and
visible mouse interactions still depend on the desktop/capture environment;
the synthetic checks do not establish those integrations.

## Alternative x64 local voice without an imported package

When voice is explicitly ON and `OVERWORLD_WHISPER_ROOT` is empty, the x64
FetchContent route downloads a hash-pinned whisper.cpp source archive at configure
time. This is an alternative to the imported clang-cl package used by the
checked releases, not a requirement for a package-only setup.
The application itself never downloads models or code.

```powershell
cmake -S . -B out\voice-x64 -G "Visual Studio 17 2022" -A x64 `
  '-DCMAKE_SYSTEM_VERSION=10.0.26100.0' `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_CONFIGURATION_TYPES=Release `
  -DOVERWORLD_VOICE_WHISPER=ON -DOVERWORLD_VOICE_AVX2=OFF
cmake --build out\voice-x64 --config Release --parallel 1 -- /nodeReuse:false
ctest --test-dir out\voice-x64 -C Release --output-on-failure
```

`OVERWORLD_VOICE_AVX2=ON` makes a distinct x64 flavor that requires
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
  -DOVERWORLD_VOICE_WHISPER=ON -DOVERWORLD_VOICE_AVX2=OFF `
  "-DOVERWORLD_WHISPER_ROOT=$package" '-DCMAKE_EXE_LINKER_FLAGS=/MAP'
cmake --build out\arm64 --config Release --parallel 1 -- /nodeReuse:false
```

Only use trusted package manifests: CMake package metadata is executable build
configuration, not an untrusted plugin format. The importer checks package
source/version, declared architecture, ABI, CRT, and per-file hashes.

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

For a copied model, verify it before use:

```powershell
$model = Get-Item .\out\voice-models\ggml-tiny.en.bin
if ($model.Length -ne 77704715 -or
    (Get-FileHash $model.FullName -Algorithm SHA256).Hash -ine
    '921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f') {
    throw 'Model does not match the pinned tiny.en bytes.'
}
```

Builds and deterministic tests do not need the weights. Actual prerecorded
inference and microphone use do; these are distinct evidence levels. A normal
app launch enables live controller handling, so it is not a build smoke test.

## Release files

Every distribution includes the repository's `LICENSE` beside `Overworld.exe`.
It grants MIT terms for original application code and documentation, not for
separately identified third-party material. The ARM64 packager includes it and
uses [the standalone release readme](RELEASE-README.md), rather than a dependency
build recipe, as the release's `README.md`.

Every flavor needs `controllers\gamecontrollerdb.txt` beside the EXE and
`controller-licenses\SDL_GameControllerDB-LICENSE.txt` from the build staged as
`licenses\SDL_GameControllerDB-LICENSE.txt`. The ARM64 packager includes both.
Never distribute a developer's `controllers\user-mappings.txt` as defaults.

Distribute the matching architecture's EXE, the exact `models\ggml-tiny.en.bin`
for voice-enabled builds, and dependency licenses/notices. Keep the model as a
separate file; do not make a ZIP or include the dependency build tree.

`PackageOverworldArm64.ps1` validates and stages a native ARM64 Release folder.
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
named third-party code/model independently of the application's MIT license.
LLVM, SDK, MSVC, static libraries, headers, PDBs, compiler logs, fixture audio, and
model caches are not application release assets.
`OverworldPractice.exe` and all probe/test executables are development-only
and must never be included in product release assets or added as product modes.

## Build identity

Windows file properties and Details include semantic version, local revision
when available, content fingerprint, target architecture/configuration, and speech
flavor. It is provenance, not a signature or proof of device testing. The build
does not contact a remote repository to generate it.

Use separate build directories for each architecture, compiler configuration,
and dependency flavor. Do not reuse a build tree after moving its source directory;
CMake caches absolute paths.
