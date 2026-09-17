# Native ARM64 Whisper

The native ARM64 application uses the same offline tiny.en model, WASAPI capture,
right-stick push-to-talk interaction, and cancellation rules as x64. Application
sources compile with MSVC; only whisper.cpp/ggml static libraries use clang-cl
with Microsoft's ARM64 ABI and a matching static CRT.

## Run a voice-enabled release

Place `GamepadDesktop.exe`, `models\ggml-tiny.en.bin`, and the supplied `licenses`
directory together. Releases use individual files, not ZIP archives.
In **Settings > Typing**, enable Local voice and Save. A blank model path uses
the default model beside the executable; a selected file must contain the exact
verified tiny.en bytes.

Focus a disposable field, open the keyboard with Y, hold right-stick click,
wait for Listening, speak, and release. View cancels. Dictation never sends Enter
automatically or redirects late output after target loss. Microphone permission
and the default input device remain controlled by Windows.

Details should report `ARM64.Release.voice-portable`. The release does not need
Whisper, LLVM, OpenMP, or Visual C++ runtime DLLs from the dependency package.
Do not distribute compiler tools, static libraries, headers, caches, or fixture
audio with the user-facing app.

## Build the dependency

Run these commands from the application root. Prerequisites:

- Visual Studio 2022 C++ and ARM64 build tools; the pinned configuration uses
  MSVC 14.44.35207 and Windows SDK 10.0.26100.0.
- CMake 3.24 or newer and PowerShell.
- LLVM 20.1.8 for Windows, including `clang-cl` and `llvm-readobj`.
  The script uses Visual Studio's bundled Ninja.

Set `$llvm` to the directory containing LLVM's `bin` directory:

```powershell
$llvm = 'C:\Tools\LLVM-20.1.8'
.\cmake\BuildWhisperLibrary.ps1 -Architecture ARM64 -LlvmRoot $llvm
```

`-VisualStudioRoot`, `-VcToolsVersion`, `-SdkVersion`, and `-BuildRoot` allow explicit
installed-tool overrides. `-WhisperArchive` accepts a previously downloaded pinned
source archive for an offline build. Otherwise the script fetches the public
hash-pinned Whisper source. It never installs tools or changes global PATH.

The package at `out\whisper-arm64-library\package` contains four static libraries,
headers, the dependency license, and a per-file hash manifest. It records the
Whisper commit, source archive SHA-256, LLVM version, target architecture, and
Microsoft ABI version.

## Link the MSVC application

```powershell
$package = (Resolve-Path .\out\whisper-arm64-library\package).Path
cmake -S . -B out\arm64 `
  -G "Visual Studio 17 2022" -A ARM64 -T "v143,version=14.44.35207" `
  '-DCMAKE_SYSTEM_VERSION=10.0.26100.0' `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_CONFIGURATION_TYPES=Release `
  -DGAMEPAD_VOICE_WHISPER=ON -DGAMEPAD_VOICE_AVX2=OFF `
  "-DGAMEPAD_WHISPER_ROOT=$package" '-DCMAKE_EXE_LINKER_FLAGS=/MAP'
cmake --build out\arm64 --config Release --target GamepadDesktop gamepad_voice_fixture
.\tests\WhisperBuildTests.ps1 -DependencyBuild out\whisper-arm64-library `
  -AppBuild out\arm64 -Architecture ARM64 -LlvmRoot $llvm
```

The importer rejects missing or changed files, wrong architecture, mismatched
source/version, incompatible ABI, non-Release configuration, and a non-static CRT.
It never turns requested voice off as a fallback.

For an equivalent x64 ABI check, build the dependency with `-Architecture x64`
and use `-A x64` plus `out\whisper-x64-library\package`. The default x64
FetchContent route remains available as described in [Building](BUILDING.md).

## Verify and stage release files

`WhisperBuildTests.ps1` checks compiler commands, every COFF member's architecture,
CRT directives, imports, and linked decoder symbols. It does not run the ARM64 app.
Use the prerecorded fixture and device checks described in [Testing](TESTING.md)
to distinguish build evidence from live microphone behavior.

After building from a committed source tree:

```powershell
.\PackageWhisperArm64.ps1 -BuildDirectory out\arm64 `
  -ModelFile .\out\voice-models\ggml-tiny.en.bin `
  -DumpbinPath 'C:\path\to\MSVC\bin\Hostx64\x64\dumpbin.exe'
```

The staging script verifies architecture, voice flavor, current source identity,
system DLL imports, and model hash. It creates a new release directory and manifest,
not an archive. See [Building](BUILDING.md#release-files) for publication contents
and licensing requirements.
