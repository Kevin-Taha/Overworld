[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$X64Package,
    [string]$TestRoot
)
$ErrorActionPreference = 'Stop'
$appRoot = Split-Path $PSScriptRoot -Parent
if (!$TestRoot) { $TestRoot = Join-Path $appRoot 'out\whisper-import-tests' }
$TestRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($TestRoot)
$X64Package = (Resolve-Path $X64Package).Path
$source = Join-Path $TestRoot 'source'
New-Item $source -ItemType Directory -Force | Out-Null
$backend = (Join-Path $appRoot 'cmake\WhisperBackend.cmake').Replace('\', '/')
@"
cmake_minimum_required(VERSION 3.24)
project(WhisperImportTests LANGUAGES CXX)
include("$backend")
if(OVERWORLD_VOICE_WHISPER AND NOT TARGET whisper)
    message(FATAL_ERROR "voice was requested but the whisper target is missing")
endif()
"@ | Set-Content (Join-Path $source 'CMakeLists.txt') -Encoding UTF8
function Configure([string]$name, [string]$arch, [string[]]$options, [string]$expectedError) {
    $build = Join-Path $TestRoot $name
    $output = & cmake -S $source -B $build -G 'Visual Studio 17 2022' -A $arch `
        '-DCMAKE_SYSTEM_VERSION=10.0.26100.0' -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
        -DCMAKE_CONFIGURATION_TYPES=Release -DOVERWORLD_VOICE_WHISPER=ON @options 2>&1
    $result = $LASTEXITCODE
    $output | Set-Content (Join-Path $TestRoot "$name.log") -Encoding UTF8
    if ($expectedError) {
        if ($result -eq 0 -or ($output -join "`n") -notmatch $expectedError) { throw "$name did not reject with $expectedError" }
    } elseif ($result -ne 0) { throw "$name configure failed: $($output -join "`n")" }
    Write-Output "PASS $name"
}
Configure 'voice-off' ARM64 @('-DOVERWORLD_VOICE_WHISPER=OFF', '-DOVERWORLD_WHISPER_ROOT=does-not-exist') ''
Configure 'arm64-no-package' ARM64 @('-DOVERWORLD_WHISPER_ROOT=') 'ARM64 voice requires the isolated'
Configure 'valid-x64' x64 @("-DOVERWORLD_WHISPER_ROOT=$X64Package") ''
Configure 'wrong-architecture' ARM64 @("-DOVERWORLD_WHISPER_ROOT=$X64Package") 'architecture/version/CRT mismatch'
Configure 'wrong-crt' x64 @("-DOVERWORLD_WHISPER_ROOT=$X64Package", '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL') 'Release /MT only'
Configure 'wrong-config' x64 @("-DOVERWORLD_WHISPER_ROOT=$X64Package", '-DCMAKE_CONFIGURATION_TYPES=Debug') 'Release /MT only'
Configure 'wrong-flavor' x64 @("-DOVERWORLD_WHISPER_ROOT=$X64Package", '-DOVERWORLD_VOICE_AVX2=ON') 'voice-portable'
Configure 'missing-package' x64 @('-DOVERWORLD_WHISPER_ROOT=does-not-exist') 'Missing Whisper package manifest'
$damaged = Join-Path $TestRoot 'damaged-package'
New-Item $damaged -ItemType Directory -Force | Out-Null
Copy-Item "$X64Package\*" $damaged -Recurse -Force
Add-Content (Join-Path $damaged 'include\whisper.h') '// deliberate test-only corruption'
Configure 'damaged-header' x64 @("-DOVERWORLD_WHISPER_ROOT=$damaged") 'integrity mismatch'
$manifestPath = Join-Path $damaged 'manifest.cmake'
$manifest = Get-Content (Join-Path $X64Package 'manifest.cmake') -Raw
($manifest -replace 'set\(GAMEPAD_WHISPER_MSVC_ABI [0-9.]+\)', 'set(GAMEPAD_WHISPER_MSVC_ABI 99.0)') |
    Set-Content $manifestPath -Encoding UTF8
Configure 'newer-abi' x64 @("-DOVERWORLD_WHISPER_ROOT=$damaged") 'MSVC toolset at least as new'
Remove-Item $damaged -Recurse -Force
