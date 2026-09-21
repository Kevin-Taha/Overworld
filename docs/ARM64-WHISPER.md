# Native ARM64 Whisper

The native ARM64 application uses the same offline tiny.en model, WASAPI capture,
right-stick push-to-talk interaction, and cancellation rules as x64. Application
sources compile with MSVC; only whisper.cpp/ggml static libraries use clang-cl
with Microsoft's ARM64 ABI and a matching static CRT.

This page covers ARM64 **target** builds and also the matching x64 dependency
package. Native ARM64 Windows hosts can build the ARM64 dependency with ARM64
LLVM and MSVC tools using the explicit recipe below. For a fresh
machine with trusted copied packages, start with
[package import](BUILDING.md#fresh-clone-and-relocated-static-packages); rebuilding
Whisper and installing LLVM are not necessary just to link those packages.

## Run a voice-enabled release

Place `Overworld.exe`, `models\ggml-tiny.en.bin`, and the supplied `licenses`
directory together, along with the supplied `controllers` mapping-data folder.
Releases use individual files, not ZIP archives. Custom controller layouts are
documented in [Controller mappings](CONTROLLERS.md).
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

Obtain LLVM 20.1.8 from the
[LLVM release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-20.1.8)
and place its complete portable distribution outside source control, for example
under `out\dependencies`. Choose the ARM64 distribution for a native ARM64 host,
or the x86_64 distribution for an x64 host. Set `$llvm` to its directory containing `bin`.
Discover the local VS installation rather than assuming an edition:

```powershell
$llvm = (Resolve-Path .\out\dependencies\llvm-20.1.8).Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -version '[17.0,18.0)' `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'VS 2022 x64 C++ tools were not found.' }
```

On an x64 host, the helper can build the dependency:

```powershell
.\cmake\BuildWhisperLibrary.ps1 -Architecture ARM64 -LlvmRoot $llvm `
  -VisualStudioRoot $vs -VcToolsVersion 14.44.35207 -SdkVersion 10.0.26100.0
```

`-VisualStudioRoot`, `-VcToolsVersion`, `-SdkVersion`, and `-BuildRoot` allow explicit
installed-tool overrides. `-WhisperArchive` accepts a previously downloaded pinned
source archive for an offline build. Otherwise the script fetches the public
hash-pinned Whisper source. It never installs tools or changes global PATH.
The `vswhere` query locates x64 C++ tools; separately confirm the specified
toolset/SDK and ARM64 target tools are installed.

**Helper limitations:** its default VS path is the Enterprise edition. It invokes
`vcvarsall.bat x64` or `x64_arm64` and defaults to an x86_64 LLVM distribution.
There is no host-architecture switch; do not use this helper for a native ARM64
build. It currently builds with `--parallel 8`; it has no `-Parallel`
parameter. This does not change the serial MSBuild app command below.
The helper also does not request `compile_commands.json` by default; the serial
recipe enables it explicitly for the full build audit.

For a serial dependency build, use the lower-level configure/build in one child
process with the matching compiler environment. This supports native ARM64-to-ARM64
and x64-to-x64/ARM64 builds.
After setting `$vs` and `$llvm` above:

```powershell
$architecture = 'ARM64' # Use 'x64' for an x64 package.
$hostArchitecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
$target = switch ("$hostArchitecture-$architecture") {
  'Arm64-ARM64' { 'arm64' }
  'X64-ARM64' { 'x64_arm64' }
  'X64-x64' { 'x64' }
  default { throw 'This recipe requires a supported host/target pair.' }
}
$build = "out\whisper-$($architecture.ToLowerInvariant())-library"
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$toolchain = (Resolve-Path .\cmake\WhisperLibrary\Toolchain.cmake).Path
# Some vcvars installations look up vswhere by name; this affects only this shell.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$command = "call `"$vcvars`" $target 10.0.26100.0 -vcvars_ver=14.44.35207 >nul" +
  " && `"$cmake`" -S cmake\WhisperLibrary -B `"$build`" -G Ninja" +
  " `"-DCMAKE_MAKE_PROGRAM=$ninja`" `"-DCMAKE_TOOLCHAIN_FILE=$toolchain`"" +
  " -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded" +
  " -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" +
  " `"-DOVERWORLD_LLVM_ROOT=$llvm`" -DOVERWORLD_WHISPER_ARCH=$architecture" +
  " && `"$cmake`" --build `"$build`" --target overworld_whisper_package --parallel 1"
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE) { throw 'Serial isolated dependency build failed.' }
```

For offline configuration, also pass
`-DOVERWORLD_WHISPER_ARCHIVE=<absolute-path-to-pinned-source-archive>` in the configure
command, or use the helper's `-WhisperArchive`. No source is downloaded if that
matching local archive is supplied. The pinned public source is:

```text
https://codeload.github.com/ggml-org/whisper.cpp/zip/a8d002cfd879315632a579e73f0148d06959de36
SHA256 4f02191b61135324a80137e459cd239e037ef3852bad4e6a2ea69490f3d89490
```

The package at `out\whisper-arm64-library\package` contains four static libraries,
headers, the dependency license, and a per-file hash manifest. It records the
Whisper commit, source archive SHA-256, LLVM version, target architecture, and
Microsoft ABI version.
Copy the entire `package` folder to relocate it, not the surrounding CMake/Ninja
build. The manifest contains package-relative file paths and hashes. Importing
still requires a compatible installed MSVC compiler and SDK; the package is not a
compiler distribution. Trust the manifest source because CMake executes it.

## Link the MSVC application

```powershell
$package = (Resolve-Path .\out\whisper-arm64-library\package).Path
cmake -S . -B out\arm64 `
  -G "Visual Studio 17 2022" -A ARM64 -T "v143,version=14.44.35207" `
  '-DCMAKE_SYSTEM_VERSION=10.0.26100.0' `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_CONFIGURATION_TYPES=Release `
  -DOVERWORLD_VOICE_WHISPER=ON -DOVERWORLD_VOICE_AVX2=OFF `
  "-DOVERWORLD_WHISPER_ROOT=$package" '-DCMAKE_EXE_LINKER_FLAGS=/MAP'
cmake --build out\arm64 --config Release --target Overworld overworld_voice_fixture --parallel 1 -- /nodeReuse:false
.\tests\WhisperBuildTests.ps1 -DependencyBuild out\whisper-arm64-library `
  -AppBuild out\arm64 -Architecture ARM64 -LlvmRoot $llvm
```

The importer rejects missing or changed files, the wrong declared architecture,
mismatched source/version, incompatible ABI, non-Release configuration, and a non-static CRT.
It never turns requested voice off as a fallback.

For an equivalent x64 ABI check, build the dependency with `-Architecture x64`
and use `-A x64` plus `out\whisper-x64-library\package`. The default x64
FetchContent route remains available as described in [Building](BUILDING.md).

## Verify and stage release files

`WhisperBuildTests.ps1` checks compiler commands, every COFF member's architecture,
CRT directives, imports, and linked decoder symbols. It does not run the ARM64 app.
It requires the original dependency build's `compile_commands.json` as well as
the app build's compiler logs and `/MAP` output. A relocated package alone does
not include that compiler-command evidence; do not call a successful package
configure a complete rebuild audit. `WhisperImportTests.ps1 -X64Package <path>`
instead tests the import contract and intentional bad-package cases in its own
scratch directory; it also requires the appropriate installed VS/SDK tools.
Use the prerecorded fixture and device checks described in [Testing](TESTING.md)
to distinguish build evidence from live microphone behavior.

After building from a committed source tree:

```powershell
$hostTools = if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString() -eq 'Arm64') {
  'Hostarm64\arm64'
} else {
  'Hostx64\x64'
}
$dumpbin = Join-Path $vs "VC\Tools\MSVC\14.44.35207\bin\$hostTools\dumpbin.exe"
.\PackageOverworldArm64.ps1 -BuildDirectory out\arm64 `
  -ModelFile .\out\voice-models\ggml-tiny.en.bin `
  -DumpbinPath $dumpbin
```

The staging script verifies architecture, voice flavor, current source identity,
system DLL imports, and model hash. It creates a new release directory and manifest,
not an archive. See [Building](BUILDING.md#release-files) for publication contents
and licensing requirements.
