param(
    [Parameter(Mandatory = $true)]
    [string]$BinaryPath,

    [string]$OutputDirectory = (Join-Path $PSScriptRoot "out/installer"),

    [string]$Version = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$resolvedBinary = (Resolve-Path -LiteralPath $BinaryPath).Path
if ([System.IO.Path]::GetExtension($resolvedBinary) -ne ".exe") {
    throw "The installer input must be a Windows .exe file: $resolvedBinary"
}

$cmake = Get-Content -LiteralPath (Join-Path $PSScriptRoot "CMakeLists.txt") -Raw
if ($cmake -notmatch 'project\(CodexMonitorHUDWindows VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not read the Windows app version from CMakeLists.txt"
}
$sourceVersion = $Matches[1]
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $sourceVersion
}
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "The MSI version must contain exactly three numeric parts: $Version"
}
$versionParts = $Version.Split('.')
try {
    [uint64]$majorVersion = [Convert]::ToUInt64($versionParts[0])
    [uint64]$minorVersion = [Convert]::ToUInt64($versionParts[1])
    [uint64]$patchVersion = [Convert]::ToUInt64($versionParts[2])
} catch {
    throw "The MSI version contains a numeric part outside the supported range: $Version"
}
if ($majorVersion -gt 255 -or $minorVersion -gt 255 -or
    $patchVersion -gt 65535) {
    throw "The MSI version exceeds Windows Installer limits (255.255.65535): $Version"
}
if ($Version -ne $sourceVersion) {
    throw "The requested MSI version $Version does not match the CMake app version $sourceVersion"
}
$binaryVersionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo(
    $resolvedBinary)
if ($binaryVersionInfo.FileVersion -ne $sourceVersion -or
    $binaryVersionInfo.ProductVersion -ne $sourceVersion) {
    throw "The input EXE version does not match CMake: expected $sourceVersion, got file=$($binaryVersionInfo.FileVersion) product=$($binaryVersionInfo.ProductVersion)"
}

$wixBin = if ($env:WIX) { Join-Path $env:WIX "bin" } else { "" }
$candle = if ($wixBin -and (Test-Path -LiteralPath (Join-Path $wixBin "candle.exe"))) {
    Join-Path $wixBin "candle.exe"
} else {
    (Get-Command candle.exe -ErrorAction Stop).Source
}
$light = if ($wixBin -and (Test-Path -LiteralPath (Join-Path $wixBin "light.exe"))) {
    Join-Path $wixBin "light.exe"
} else {
    (Get-Command light.exe -ErrorAction Stop).Source
}

$source = (Resolve-Path -LiteralPath (
    Join-Path $PSScriptRoot "installer/CodexMonitorHUD.wxs")).Path
$icon = (Resolve-Path -LiteralPath (
    Join-Path $PSScriptRoot "resources/CodexMonitorHUD.ico")).Path
$license = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "../LICENSE")).Path
$readme = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "README.md")).Path
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
$intermediate = Join-Path $resolvedOutput "obj"
$objectPath = Join-Path $intermediate "CodexMonitorHUD.wixobj"
$installerName = "CodexMonitorHUD-windows-x64-$Version.msi"
$installerPath = Join-Path $resolvedOutput $installerName
$checksumPath = "$installerPath.sha256"

New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null
if (Test-Path -LiteralPath $intermediate) {
    Remove-Item -LiteralPath $intermediate -Recurse -Force
}
New-Item -ItemType Directory -Path $intermediate | Out-Null
foreach ($stale in @($installerPath, $checksumPath)) {
    if (Test-Path -LiteralPath $stale) {
        Remove-Item -LiteralPath $stale -Force
    }
}

& $candle -nologo -arch x64 `
    "-dAppVersion=$Version" `
    "-dBinaryPath=$resolvedBinary" `
    "-dIconPath=$icon" `
    "-dLicensePath=$license" `
    "-dReadmePath=$readme" `
    -out $objectPath $source
if ($LASTEXITCODE -ne 0) {
    throw "WiX compiler failed with exit code $LASTEXITCODE"
}

& $light -nologo -out $installerPath $objectPath
if ($LASTEXITCODE -ne 0) {
    throw "WiX linker failed with exit code $LASTEXITCODE"
}

$installer = Get-Item -LiteralPath $installerPath
if ($installer.Length -le 0) {
    throw "The Windows MSI installer is empty: $installerPath"
}
$hash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText(
    $checksumPath,
    "$hash  $($installer.Name)`n",
    [System.Text.Encoding]::ASCII)

Write-Output "installer=$installerPath"
Write-Output "checksum=$checksumPath"
Write-Output "sha256=$hash"
