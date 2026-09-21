[CmdletBinding()]
param(
    [ValidateSet('ARM64', 'x64')][string]$Architecture = 'ARM64',
    [string]$LlvmRoot,
    [string]$BuildRoot,
    [string]$WhisperArchive,
    [string]$VisualStudioRoot = 'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
    [string]$SdkVersion = '10.0.26100.0',
    [string]$VcToolsVersion = '14.44.35207'
)
$ErrorActionPreference = 'Stop'
$appRoot = Split-Path $PSScriptRoot -Parent
if (!$LlvmRoot) {
    $LlvmRoot = Join-Path $appRoot 'out\dependencies\llvm-20.1.8\clang+llvm-20.1.8-x86_64-pc-windows-msvc'
}
if (!$BuildRoot) {
    $BuildRoot = Join-Path $appRoot ("out\whisper-{0}-library" -f $Architecture.ToLowerInvariant())
}
$LlvmRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LlvmRoot)
$BuildRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildRoot)
$clang = Join-Path $LlvmRoot 'bin\clang-cl.exe'
$vcvars = Join-Path $VisualStudioRoot 'VC\Auxiliary\Build\vcvarsall.bat'
$ninja = Join-Path $VisualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
foreach ($path in @($clang, $vcvars, $ninja)) {
    if (!(Test-Path $path -PathType Leaf)) { throw "Missing tool: $path. Supply a portable LLVM 20.1.8 root and VS2022 C++ tools; no global installation is performed." }
}
$version = & $clang --version
if ($LASTEXITCODE -ne 0 -or $version[0] -notmatch '^clang version 20\.1\.8(?:\s|$)') {
    throw 'The isolated Whisper library requires clang-cl 20.1.8.'
}
Write-Output $version
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$target = 'x64'
if ($Architecture -eq 'ARM64') { $target = 'x64_arm64' }
$arguments = @(
    '-S', (Join-Path $PSScriptRoot 'WhisperLibrary'), '-B', $BuildRoot, '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $PSScriptRoot 'WhisperLibrary\Toolchain.cmake')",
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    "-DOVERWORLD_LLVM_ROOT=$LlvmRoot", "-DOVERWORLD_WHISPER_ARCH=$Architecture"
)
if ($WhisperArchive) { $arguments += "-DOVERWORLD_WHISPER_ARCHIVE=$((Resolve-Path $WhisperArchive).Path)" }
# vcvars is confined to this child cmd process; no persistent PATH or settings changes.
$quoted = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
$command = "call `"$vcvars`" $target $SdkVersion -vcvars_ver=$VcToolsVersion >nul && `"$cmake`" $quoted && `"$cmake`" --build `"$BuildRoot`" --target overworld_whisper_package --parallel 8"
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Isolated Whisper $Architecture build failed (exit $LASTEXITCODE)." }
Write-Output "Whisper package: $(Join-Path $BuildRoot 'package')"
