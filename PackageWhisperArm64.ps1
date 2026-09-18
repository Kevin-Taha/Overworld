param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$ModelFile,
    [Parameter(Mandatory = $true)][string]$DumpbinPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$exe = Get-Item -LiteralPath (Join-Path $build 'Release\GamepadDesktop.exe')
$identity = $exe.VersionInfo.ProductVersion
if ($identity -notmatch '\+git\.([a-f0-9]+)\.src\.[a-f0-9]+\.ARM64\.Release\.voice-portable$') {
    throw 'Expected a versioned native ARM64 Release Whisper executable, not voice-off or emulation.'
}
$revision = $Matches[1]
$head = & git -C $PSScriptRoot rev-parse HEAD
if ($LASTEXITCODE -or -not $head.StartsWith($revision)) { throw 'Rebuild the app from current source HEAD.' }
$status = & git -C $PSScriptRoot status --porcelain -- .
if ($LASTEXITCODE) { throw 'Could not verify source status.' }
if ($status) { throw 'Commit app source changes before packaging.' }
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
if ($cache -notmatch '(?m)^GAMEPAD_VOICE_WHISPER:BOOL=ON\r?$') {
    throw 'The build must explicitly enable Whisper.'
}
$reader = [IO.BinaryReader]::new([IO.File]::OpenRead($exe.FullName))
try {
    if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Invalid executable DOS header.' }
    $reader.BaseStream.Position = 0x3C
    $offset = $reader.ReadInt32()
    $reader.BaseStream.Position = $offset
    if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0xAA64) {
        throw 'Executable is not native ARM64.'
    }
} finally {
    $reader.Dispose()
}
$listing = & $DumpbinPath /nologo /dependents $exe.FullName
if ($LASTEXITCODE) { throw 'Executable import inspection failed.' }
$imports = @($listing | Where-Object { $_ -match '^\s+[\w.-]+\.dll\s*$' } |
    ForEach-Object { $_.Trim().ToLowerInvariant() } | Sort-Object -Unique)
if (-not $imports.Count) { throw 'Executable import inspection returned no DLLs.' }
foreach ($dependency in $imports) {
    if ($dependency -match '^(vcruntime|msvcp|concrt|vccorlib|libomp|whisper|ggml|clang_rt)' -or
        ($dependency -notmatch '^(api-ms-|ext-ms-)' -and
         -not (Test-Path -LiteralPath (Join-Path "$env:WINDIR\System32" $dependency) -PathType Leaf))) {
        throw "Unbundled non-system dependency: $dependency"
    }
}
$model = Get-Item -LiteralPath $ModelFile
if ($model.Length -ne 77704715 -or
    (Get-FileHash -LiteralPath $model.FullName -Algorithm SHA256).Hash -ine
    '921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f') {
    throw 'Model does not match the pinned Whisper tiny.en weights.'
}
$license = Join-Path $build 'voice-licenses\whisper.cpp-LICENSE.txt'
$notice = Join-Path $PSScriptRoot 'third_party\voice-notices.txt'
$predictionNotice = Join-Path $PSScriptRoot 'third_party\prediction-notices.txt'
$predictionLicense = Join-Path $PSScriptRoot 'third_party\prediction-CC-BY-SA-4.0.txt'
$readme = Join-Path $PSScriptRoot 'docs\ARM64-WHISPER.md'
$controllerDatabase = Join-Path $build 'Release\controllers\gamecontrollerdb.txt'
$controllerLicense = Join-Path $build 'controller-licenses\SDL_GameControllerDB-LICENSE.txt'
$controllerGuide = Join-Path $PSScriptRoot 'docs\CONTROLLERS.md'
foreach ($path in @($license, $notice, $predictionNotice, $predictionLicense, $readme, $controllerDatabase, $controllerLicense, $controllerGuide)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing package input: $path" }
}
if ((Get-FileHash -LiteralPath $controllerDatabase -Algorithm SHA256).Hash -ine
    '07ec5b753e685c4829987919b27905cc3c45c8e18c11b3046e275fcf38b0f3cb' -or
    (Get-FileHash -LiteralPath $controllerLicense -Algorithm SHA256).Hash -ine
    '97f35b302b361680ec1e891e95d2d52097bb95abff361434916d99dc1305f127') {
    throw 'Controller mapping release inputs do not match their pins.'
}
$bundle = Join-Path $build "packages\GamepadDesktop-ARM64-Whisper-$revision-$(Get-Date -Format yyyyMMdd-HHmmss)"
if (Test-Path -LiteralPath $bundle) { throw 'Refusing to overwrite an existing package.' }
New-Item -ItemType Directory -Path "$bundle\models", "$bundle\licenses", "$bundle\controllers" | Out-Null
Copy-Item -LiteralPath $exe.FullName -Destination "$bundle\GamepadDesktop.exe"
Copy-Item -LiteralPath $model.FullName -Destination "$bundle\models\ggml-tiny.en.bin"
Copy-Item -LiteralPath $license, $notice, $predictionNotice, $predictionLicense, $controllerLicense -Destination "$bundle\licenses"
Copy-Item -LiteralPath $controllerDatabase -Destination "$bundle\controllers\gamecontrollerdb.txt"
Copy-Item -LiteralPath $controllerGuide -Destination "$bundle\CONTROLLERS.md"
Copy-Item -LiteralPath $readme -Destination "$bundle\README.md"
$files = @(Get-ChildItem -LiteralPath $bundle -Recurse -File | ForEach-Object {
    [ordered]@{
        path = $_.FullName.Substring($bundle.Length + 1)
        bytes = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
[ordered]@{
    entrypoint = 'GamepadDesktop.exe'
    identity = $identity
    sourceCommit = $head
    backend = 'whisper.cpp v1.7.6; tiny.en; native ARM64'
    sourceDirty = $false
    machine = '0xAA64'
    imports = $imports
    liveMicrophoneIntegrated = $true
    armRuntimeVerified = $false
    microphoneTested = $false
    files = $files
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$bundle\manifest.json" -Encoding UTF8
Write-Output "Full app: $bundle\GamepadDesktop.exe"
Write-Output "Model: $bundle\models\ggml-tiny.en.bin"
Write-Output "Licenses: $bundle\licenses"
Write-Output "Manifest: $bundle\manifest.json"
Write-Output 'Publish these files individually. No ZIP archive was created.'
