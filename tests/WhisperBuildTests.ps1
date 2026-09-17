[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DependencyBuild,
    [Parameter(Mandatory)][string]$AppBuild,
    [ValidateSet('ARM64', 'x64')][string]$Architecture = 'ARM64',
    [string]$LlvmRoot
)
$ErrorActionPreference = 'Stop'
$appRoot = Split-Path $PSScriptRoot -Parent
if (!$LlvmRoot) {
    $LlvmRoot = Join-Path $appRoot 'out\dependencies\llvm-20.1.8\clang+llvm-20.1.8-x86_64-pc-windows-msvc'
}
$reader = Join-Path $LlvmRoot 'bin\llvm-readobj.exe'
$DependencyBuild = (Resolve-Path $DependencyBuild).Path
$AppBuild = (Resolve-Path $AppBuild).Path
$report = [Collections.Generic.List[string]]::new()
function Require([bool]$condition, [string]$description) {
    if (!$condition) { throw $description }
    $report.Add("PASS $description")
}
$expectedMachine = '0xAA64'
$triple = 'aarch64-pc-windows-msvc'
if ($Architecture -eq 'x64') { $expectedMachine = '0x8664'; $triple = 'x86_64-pc-windows-msvc' }
$commands = Get-Content (Join-Path $DependencyBuild 'compile_commands.json') -Raw | ConvertFrom-Json
foreach ($entry in $commands) {
    Require ($entry.command -match 'clang-cl\.exe' -and $entry.command.Contains("--target=$triple")) "dependency compiler/target: $($entry.file)"
    Require ($entry.command -match '(?:^|\s)[/-]MT(?:\s|$)' -and $entry.command -notmatch '(?i)(?:flto|/GL(?:\s|$)|mcpu=native|march=native|/arch:AVX)') "dependency static CRT/baseline/no LTO: $($entry.file)"
}
$compilerFile = Get-ChildItem (Join-Path $AppBuild 'CMakeFiles') -Filter CMakeCXXCompiler.cmake -Recurse | Select-Object -First 1
$compiler = Get-Content $compilerFile.FullName -Raw
Require ($compiler -match 'set\(CMAKE_CXX_COMPILER_ID "MSVC"\)' -and $compiler -match '[/\\]cl\.exe"') 'application compiler is real MSVC cl.exe'
$appCommands = @(Get-ChildItem $AppBuild -Filter CL.command.1.tlog -Recurse |
    Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' })
Require ($appCommands.Count -gt 0) 'application compiler command evidence exists'
foreach ($file in $appCommands) {
    $text = Get-Content $file.FullName -Raw
    Require ($text -match '/MT(?:\s|$)' -and $text -match '/W4(?:\s|$)' -and $text -match '/WX(?:\s|$)' -and $text -match '/permissive-' -and $text -match '/utf-8') "application warning/CRT contract: $($file.FullName)"
}
$binaries = @(
    Get-ChildItem (Join-Path $DependencyBuild 'package\lib') -Filter '*.lib'
    Get-ChildItem $AppBuild -Recurse -File |
        Where-Object { $_.Extension -in @('.obj', '.lib', '.exe') -and $_.FullName -notmatch '\\CMakeFiles\\' }
)
foreach ($file in $binaries) {
    $output = & $reader --file-headers --coff-directives $file.FullName 2>&1
    Require ($LASTEXITCODE -eq 0) "COFF readable: $($file.FullName)"
    $machines = @($output | Select-String 'Machine: ')
    Require ($machines.Count -gt 0 -and @($machines | Where-Object { !$_.Line.Contains("($expectedMachine)") }).Count -eq 0) "all members $expectedMachine`: $($file.FullName)"
    Require (!(($output -join "`n") -match '(?i)(defaultlib:(?:"?)(msvcrt|msvcprt)|RuntimeLibrary=MD_|RuntimeLibrary=MTd_)')) "no dynamic/debug CRT directives: $($file.FullName)"
    if ($file.Extension -eq '.exe') {
        $imports = & $reader --coff-imports $file.FullName 2>&1
        Require ($LASTEXITCODE -eq 0) "imports readable: $($file.FullName)"
        $names = @($imports | Select-String '^  Name: ')
        Require (!(($names -join "`n") -match '(?i)(vcruntime|msvcp|ucrtbase|libomp|libiomp|clang|llvm|whisper|ggml).*\.dll')) "no VC/LLVM/Whisper/OpenMP runtime DLLs: $($file.FullName)"
        foreach ($name in $names) { $report.Add($name.Line.Trim()) }
    }
}
$exe = Join-Path $AppBuild 'Release\GamepadDesktop.exe'
$version = (Get-Item $exe).VersionInfo.ProductVersion
Require ($version.EndsWith(".$Architecture.Release.voice-portable")) "voice-enabled native build identity: $version"
$map = Get-Content (Join-Path $AppBuild 'Release\GamepadDesktop.map') -Raw
Require ($map -match 'whisper_init_from_buffer_with_params\s+[0-9a-f]+\s+f\s+whisper:' -and $map -match 'whisper_full\s+[0-9a-f]+\s+f\s+whisper:') 'Whisper initialization and decode symbols linked from static backend'
$reportPath = Join-Path $AppBuild 'whisper-build-audit.txt'
$report | Set-Content $reportPath -Encoding UTF8
Write-Output "PASS: $($commands.Count) dependency compile commands, $($binaries.Count) native binaries/objects, compiler/CRT/import/symbol/build-identity checks. Report: $reportPath"
