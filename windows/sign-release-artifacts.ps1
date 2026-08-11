param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Fingerprint", "SignAndVerify")]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [string]$CertificatePath,

    [Parameter(Mandatory = $true)]
    [string]$CertificatePassword,

    [string[]]$FilePath = @(),

    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$resolvedCertificate = (Resolve-Path -LiteralPath $CertificatePath).Path
$securePassword = ConvertTo-SecureString $CertificatePassword -AsPlainText -Force
$certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
    $resolvedCertificate,
    $securePassword,
    [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
try {
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $publisherFingerprint = -join (
            $sha256.ComputeHash($certificate.RawData) |
                ForEach-Object { $_.ToString("x2") })
    } finally {
        $sha256.Dispose()
    }
    if ($publisherFingerprint.Length -ne 64) {
        throw "The release certificate SHA-256 fingerprint is invalid"
    }
    $leafThumbprint = $certificate.Thumbprint
    if ([string]::IsNullOrWhiteSpace($leafThumbprint)) {
        throw "The release certificate has no SHA-1 store identity"
    }
    if (-not $certificate.HasPrivateKey) {
        throw "The release certificate does not contain a private key"
    }
    $codeSigningOid = "1.3.6.1.5.5.7.3.3"
    $hasCodeSigningUsage = $false
    foreach ($extension in $certificate.Extensions) {
        if ($extension -is [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]) {
            foreach ($oid in $extension.EnhancedKeyUsages) {
                if ($oid.Value -eq $codeSigningOid) {
                    $hasCodeSigningUsage = $true
                }
            }
        }
    }
    if (-not $hasCodeSigningUsage) {
        throw "The release certificate is not valid for code signing"
    }
    if ($Action -eq "Fingerprint") {
        Write-Output $publisherFingerprint
        return
    }
} finally {
    $certificate.Dispose()
}

if ($FilePath.Count -eq 0) {
    throw "SignAndVerify requires at least one release file"
}
if ($TimestampUrl -notmatch '^https?://[A-Za-z0-9.-]+(?::[0-9]+)?(?:/.*)?$') {
    throw "The timestamp URL is invalid"
}

$windowsKitsBin = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
$signTool = Get-ChildItem -LiteralPath $windowsKitsBin -Directory |
    Where-Object { $_.Name -match '^\d+(\.\d+){3}$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object {
        $candidate = Join-Path $_.FullName "x64\signtool.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $candidate
        }
    } |
    Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($signTool)) {
    throw "Could not find the x64 Windows SDK signtool.exe"
}

$importedCertificates = @()
$importedCertificate = $null
try {
    $importedCertificates = @(Import-PfxCertificate `
        -FilePath $resolvedCertificate `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -Password $securePassword `
        -Exportable:$false)
    $leafStorePath = "Cert:\CurrentUser\My\$leafThumbprint"
    if (-not (Test-Path -LiteralPath $leafStorePath)) {
        throw "The release certificate could not be imported"
    }
    $importedCertificate = Get-Item -LiteralPath $leafStorePath

    foreach ($path in $FilePath) {
        $resolvedFile = (Resolve-Path -LiteralPath $path).Path
        & $signTool sign /fd SHA256 /td SHA256 /tr $TimestampUrl `
            /sha1 $importedCertificate.Thumbprint /s My $resolvedFile
        if ($LASTEXITCODE -ne 0) {
            throw "Authenticode signing failed for $resolvedFile"
        }
        & $signTool verify /pa /all /v $resolvedFile
        if ($LASTEXITCODE -ne 0) {
            throw "Authenticode verification failed for $resolvedFile"
        }
    }
} finally {
    foreach ($imported in $importedCertificates) {
        if ($null -eq $imported -or
            [string]::IsNullOrWhiteSpace($imported.Thumbprint)) {
            continue
        }
        $storePath = "Cert:\CurrentUser\My\$($imported.Thumbprint)"
        if (Test-Path -LiteralPath $storePath) {
            Remove-Item -LiteralPath $storePath -DeleteKey -Force
        }
        $imported.Dispose()
    }
}

Write-Output "publisher_sha256=$publisherFingerprint"
Write-Output "signed_files=$($FilePath.Count)"
