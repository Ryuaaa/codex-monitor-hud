param(
    [Parameter(Mandatory = $true)]
    [string]$BinaryPath,

    [string]$OutputDirectory = (Join-Path $PSScriptRoot "out/release")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$resolvedBinary = (Resolve-Path -LiteralPath $BinaryPath).Path
if ([System.IO.Path]::GetExtension($resolvedBinary) -ne ".exe") {
    throw "The release input must be a Windows .exe file: $resolvedBinary"
}

$licensePath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "../LICENSE")).Path
$readmePath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "README.md")).Path
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
$packageName = "CodexMonitorHUD-windows-x64"
$stagingDirectory = Join-Path $resolvedOutput $packageName
$archivePath = Join-Path $resolvedOutput "$packageName.zip"
$checksumPath = "$archivePath.sha256"

New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null
if (Test-Path -LiteralPath $stagingDirectory) {
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
}
if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}
if (Test-Path -LiteralPath $checksumPath) {
    Remove-Item -LiteralPath $checksumPath -Force
}

New-Item -ItemType Directory -Path $stagingDirectory | Out-Null
Copy-Item -LiteralPath $resolvedBinary -Destination (
    Join-Path $stagingDirectory "CodexMonitorHUD.exe")
Copy-Item -LiteralPath $licensePath -Destination (
    Join-Path $stagingDirectory "LICENSE")
Copy-Item -LiteralPath $readmePath -Destination (
    Join-Path $stagingDirectory "README.md")

Compress-Archive -LiteralPath $stagingDirectory `
    -DestinationPath $archivePath -CompressionLevel Optimal

$archive = Get-Item -LiteralPath $archivePath
if ($archive.Length -le 0) {
    throw "The Windows release archive is empty: $archivePath"
}

$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumLine = "$hash  $($archive.Name)`n"
[System.IO.File]::WriteAllText(
    $checksumPath,
    $checksumLine,
    [System.Text.Encoding]::ASCII)

Write-Output "archive=$archivePath"
Write-Output "checksum=$checksumPath"
Write-Output "sha256=$hash"
