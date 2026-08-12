[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CurrentBinaryPath,

    [Parameter(Mandatory = $true)]
    [string]$CurrentInstallerPath,

    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string]$ExpectedPublisherSha256 = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$previousCommit = "2c2a103534596b1f191d6e9475b32738794bf9a2"
$expectedPreviousVersion = "0.2.0"
$expectedUpgradeCode = "{0CA9E00B-2AAF-4393-B466-1AF0F8C2C21F}"
$successExitCodes = @(0, 3010)
$absentProductExitCodes = @(1605, 1614)
$expectedDowngradeMessage = "A newer version of Codex Monitor HUD is already installed."

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class CodexMonitorMsiNative
{
    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    public static extern uint MsiEnumRelatedProducts(
        string upgradeCode,
        uint reserved,
        uint productIndex,
        StringBuilder productCode);
}
'@

function ConvertTo-CanonicalGuid {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    try {
        return ([guid]$Value).ToString("B").ToUpperInvariant()
    } catch {
        throw "$Description is not a valid GUID: $Value"
    }
}

function Get-MsiProperty {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallerPath,

        [Parameter(Mandatory = $true)]
        [ValidatePattern('^[A-Za-z][A-Za-z0-9_]*$')]
        [string]$PropertyName
    )

    $installer = $null
    $database = $null
    $view = $null
    $record = $null
    $duplicateRecord = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $database = $installer.GetType().InvokeMember(
            "OpenDatabase",
            [Reflection.BindingFlags]::InvokeMethod,
            $null,
            $installer,
            [object[]]@($InstallerPath, 0))
        $query = "SELECT ``Value`` FROM ``Property`` WHERE ``Property``='$PropertyName'"
        $view = $database.GetType().InvokeMember(
            "OpenView",
            [Reflection.BindingFlags]::InvokeMethod,
            $null,
            $database,
            [object[]]@($query))
        [void]$view.GetType().InvokeMember(
            "Execute",
            [Reflection.BindingFlags]::InvokeMethod,
            $null,
            $view,
            [object[]]@())
        $record = $view.GetType().InvokeMember(
            "Fetch",
            [Reflection.BindingFlags]::InvokeMethod,
            $null,
            $view,
            [object[]]@())
        if ($null -eq $record) {
            throw "MSI property is missing: $PropertyName"
        }
        $value = [string]$record.GetType().InvokeMember(
            "StringData",
            [Reflection.BindingFlags]::GetProperty,
            $null,
            $record,
            [object[]]@(1))
        if ([string]::IsNullOrWhiteSpace($value)) {
            throw "MSI property is empty: $PropertyName"
        }
        $duplicateRecord = $view.GetType().InvokeMember(
            "Fetch",
            [Reflection.BindingFlags]::InvokeMethod,
            $null,
            $view,
            [object[]]@())
        if ($null -ne $duplicateRecord) {
            throw "MSI property is duplicated: $PropertyName"
        }
        return $value
    } finally {
        if ($null -ne $view) {
            try {
                [void]$view.GetType().InvokeMember(
                    "Close",
                    [Reflection.BindingFlags]::InvokeMethod,
                    $null,
                    $view,
                    [object[]]@())
            } catch {
                Write-Verbose "Could not explicitly close the MSI database view"
            }
        }
        foreach ($item in @(
                $duplicateRecord, $record, $view, $database, $installer)) {
            if ($null -ne $item -and
                [Runtime.InteropServices.Marshal]::IsComObject($item)) {
                [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($item)
            }
        }
    }
}

function Get-RelatedProductCodes {
    param(
        [Parameter(Mandatory = $true)]
        [string]$UpgradeCode
    )

    $canonicalUpgradeCode = ConvertTo-CanonicalGuid `
        -Value $UpgradeCode -Description "UpgradeCode"
    $products = [System.Collections.Generic.List[string]]::new()
    for ([uint32]$index = 0; $index -lt 64; $index++) {
        $buffer = [Text.StringBuilder]::new(39)
        $status = [CodexMonitorMsiNative]::MsiEnumRelatedProducts(
            $canonicalUpgradeCode, 0, $index, $buffer)
        if ($status -eq 259) {
            return @($products.ToArray() | Sort-Object -Unique)
        }
        if ($status -ne 0) {
            throw "MsiEnumRelatedProducts failed with status $status"
        }
        $products.Add((ConvertTo-CanonicalGuid `
            -Value $buffer.ToString() -Description "Related ProductCode"))
    }
    throw "More than 64 related MSI products were registered"
}

function Invoke-MsiExec {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("install", "uninstall")]
        [string]$Operation,

        [Parameter(Mandatory = $true)]
        [string]$Target,

        [Parameter(Mandatory = $true)]
        [string]$LogPath,

        [string]$InstallFolder = ""
    )

    $verb = if ($Operation -eq "install") { "/i" } else { "/x" }
    $arguments = "$verb `"$Target`" /qn /norestart"
    if (-not [string]::IsNullOrWhiteSpace($InstallFolder)) {
        $arguments += " INSTALLFOLDER=`"$InstallFolder`""
    }
    $arguments += " /L*v `"$LogPath`""

    for ($attempt = 1; $attempt -le 4; $attempt++) {
        $process = Start-Process -FilePath "msiexec.exe" -Wait -PassThru `
            -ArgumentList $arguments
        if ($process.ExitCode -ne 1618 -or $attempt -eq 4) {
            return $process.ExitCode
        }
        Start-Sleep -Seconds 2
    }
    throw "The MSI retry loop ended unexpectedly"
}

function Show-MsiLogs {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory
    )

    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        return
    }
    foreach ($log in Get-ChildItem -LiteralPath $Directory -Filter "*.log" -File) {
        Write-Host "--- $($log.Name) (last 120 lines) ---"
        Get-Content -LiteralPath $log.FullName -Tail 120
    }
}

function Replace-ExactlyOnce {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [string]$Pattern,

        [Parameter(Mandatory = $true)]
        [string]$Replacement,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $matches = [regex]::Matches($Text, $Pattern)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one $Description match, found $($matches.Count)"
    }
    $literalReplacement = $Replacement.Replace('$', '$$')
    return ([regex]::new($Pattern)).Replace($Text, $literalReplacement, 1)
}

function Get-NormalizedPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $trailingSeparators = [char[]]@(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    return ([IO.Path]::GetFullPath($Path)).TrimEnd($trailingSeparators)
}

function Get-TrustedPublisherSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne "Valid" -or
        $null -eq $signature.SignerCertificate) {
        throw "File does not have a valid trusted Authenticode signature: $Path"
    }
    return ($signature.SignerCertificate.GetCertHashString(
        [Security.Cryptography.HashAlgorithmName]::SHA256)).ToLowerInvariant()
}

$repositoryRoot = (Resolve-Path -LiteralPath (
    Join-Path $PSScriptRoot "../..")).Path
$resolvedCurrentBinary = (Resolve-Path -LiteralPath $CurrentBinaryPath).Path
$resolvedCurrentInstaller = (Resolve-Path -LiteralPath $CurrentInstallerPath).Path
$expectedPublisher = $ExpectedPublisherSha256.ToLowerInvariant()
if ([IO.Path]::GetExtension($resolvedCurrentBinary) -ne ".exe") {
    throw "Current binary must be an EXE: $resolvedCurrentBinary"
}
if ([IO.Path]::GetExtension($resolvedCurrentInstaller) -ne ".msi") {
    throw "Current installer must be an MSI: $resolvedCurrentInstaller"
}

$temporaryBase = if (-not [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    [IO.Path]::GetFullPath($env:RUNNER_TEMP)
} else {
    [IO.Path]::GetTempPath()
}
$testRoot = Join-Path $temporaryBase (
    "codex-monitor-msi-major-upgrade-" + ([guid]::NewGuid()).ToString("N"))
$logsDirectory = Join-Path $testRoot "logs"
$oldArchive = Join-Path $testRoot "previous-source.zip"
$oldSource = Join-Path $testRoot "previous-source"
$oldWindows = Join-Path $oldSource "windows"
$oldBuild = Join-Path $oldWindows "out/build"
$oldInstallerOutput = Join-Path $oldWindows "out/installer"
$installDirectory = Join-Path $testRoot "custom-install/Codex Monitor HUD"
$installedBinary = Join-Path $installDirectory "CodexMonitorHUD.exe"
$preservedMarker = Join-Path $installDirectory "user-preserved.txt"

$oldProductCode = ""
$currentProductCode = ""
$cleanupOldProduct = $false
$cleanupCurrentProduct = $false
$verifyNoProductsAfterCleanup = $false
$primaryFailure = $null
$cleanupFailures = [System.Collections.Generic.List[string]]::new()

New-Item -ItemType Directory -Path $logsDirectory -Force | Out-Null

try {
    Write-Host "phase=preflight start"
    & git -C $repositoryRoot cat-file -e "$previousCommit`^{commit}"
    if ($LASTEXITCODE -ne 0) {
        throw "Pinned previous commit is unavailable: $previousCommit"
    }

    $currentVersion = Get-MsiProperty `
        -InstallerPath $resolvedCurrentInstaller -PropertyName "ProductVersion"
    $currentProductCode = ConvertTo-CanonicalGuid `
        -Value (Get-MsiProperty -InstallerPath $resolvedCurrentInstaller `
            -PropertyName "ProductCode") `
        -Description "Current ProductCode"
    $currentUpgradeCode = ConvertTo-CanonicalGuid `
        -Value (Get-MsiProperty -InstallerPath $resolvedCurrentInstaller `
            -PropertyName "UpgradeCode") `
        -Description "Current UpgradeCode"
    if ($currentUpgradeCode -ne $expectedUpgradeCode) {
        throw "Current MSI has an unexpected UpgradeCode: $currentUpgradeCode"
    }
    if ([version]$currentVersion -le [version]$expectedPreviousVersion) {
        throw "Current MSI version $currentVersion must exceed $expectedPreviousVersion"
    }
    $currentVersionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo(
        $resolvedCurrentBinary)
    if ($currentVersionInfo.FileVersion -ne $currentVersion -or
        $currentVersionInfo.ProductVersion -ne $currentVersion) {
        throw "Current EXE and MSI versions do not match: MSI=$currentVersion file=$($currentVersionInfo.FileVersion) product=$($currentVersionInfo.ProductVersion)"
    }
    if (-not [string]::IsNullOrWhiteSpace($expectedPublisher)) {
        $binaryPublisher = Get-TrustedPublisherSha256 `
            -Path $resolvedCurrentBinary
        $installerPublisher = Get-TrustedPublisherSha256 `
            -Path $resolvedCurrentInstaller
        if ($binaryPublisher -ne $expectedPublisher -or
            $installerPublisher -ne $expectedPublisher) {
            throw "Current EXE or MSI publisher does not match the expected certificate"
        }
    }
    $preexistingProducts = @(Get-RelatedProductCodes `
        -UpgradeCode $expectedUpgradeCode)
    if ($preexistingProducts.Count -ne 0) {
        throw "Upgrade test refuses to modify an existing Codex Monitor HUD installation"
    }
    $verifyNoProductsAfterCleanup = $true
    Write-Host "phase=preflight done current_version=$currentVersion"

    Write-Host "phase=previous-build start"
    & git -C $repositoryRoot archive --format=zip `
        --output=$oldArchive $previousCommit
    if ($LASTEXITCODE -ne 0 -or
        -not (Test-Path -LiteralPath $oldArchive -PathType Leaf)) {
        throw "Could not export the pinned previous source"
    }
    Expand-Archive -LiteralPath $oldArchive -DestinationPath $oldSource

    Copy-Item -LiteralPath (Join-Path $repositoryRoot "windows/resources") `
        -Destination $oldWindows -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "windows/installer") `
        -Destination $oldWindows -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $repositoryRoot `
        "windows/build-installer.ps1") -Destination $oldWindows -Force

    $oldCmakePath = Join-Path $oldWindows "CMakeLists.txt"
    $oldCmake = Get-Content -LiteralPath $oldCmakePath -Raw
    if ($oldCmake -notmatch [regex]::Escape(
        "project(CodexMonitorHUDWindows VERSION $expectedPreviousVersion")) {
        throw "Pinned previous source no longer has version $expectedPreviousVersion"
    }
    $oldCmake = Replace-ExactlyOnce -Text $oldCmake `
        -Pattern 'include\(CTest\)' `
        -Replacement "include(CTest)`n`nif(WIN32)`n    enable_language(RC)`nendif()" `
        -Description "CTest include"
    $oldIncludeDirectories = @(
        'target_include_directories(${target} PRIVATE',
        '        ${CMAKE_CURRENT_SOURCE_DIR}/src',
        '        ${CMAKE_CURRENT_SOURCE_DIR}/resources',
        '    )'
    ) -join "`n"
    $oldCmake = Replace-ExactlyOnce -Text $oldCmake `
        -Pattern 'target_include_directories\(\$\{target\} PRIVATE \$\{CMAKE_CURRENT_SOURCE_DIR\}/src\)' `
        -Replacement $oldIncludeDirectories `
        -Description "target include directory"
    $oldMsvcOptions = @(
        'target_compile_options(${target} PRIVATE',
        '            $<$<COMPILE_LANGUAGE:CXX>:/W4>',
        '            $<$<COMPILE_LANGUAGE:CXX>:/permissive->',
        '            $<$<COMPILE_LANGUAGE:CXX>:/utf-8>',
        '            $<$<COMPILE_LANGUAGE:CXX>:/EHsc>',
        '        )'
    ) -join "`n"
    $oldCmake = Replace-ExactlyOnce -Text $oldCmake `
        -Pattern 'target_compile_options\(\$\{target\} PRIVATE /W4 /permissive- /utf-8 /EHsc\)' `
        -Replacement $oldMsvcOptions `
        -Description "MSVC C++ compile options"
    $oldCmake = Replace-ExactlyOnce -Text $oldCmake `
        -Pattern 'src/windows_sampler\.cpp\r?\n    \)' `
        -Replacement "src/windows_sampler.cpp`n        resources/CodexMonitorHUD.rc`n    )" `
        -Description "HUD source list"
    [IO.File]::WriteAllText($oldCmakePath, $oldCmake,
        [Text.UTF8Encoding]::new($false))

    $oldVersionParts = $expectedPreviousVersion.Split('.')
    $oldResourcePath = Join-Path $oldWindows `
        "resources/CodexMonitorHUD.rc"
    $oldResource = Get-Content -LiteralPath $oldResourcePath -Raw
    $oldResource = Replace-ExactlyOnce -Text $oldResource `
        -Pattern '#define CODEX_MONITOR_WINDOWS_VERSION "[0-9]+\.[0-9]+\.[0-9]+"' `
        -Replacement "#define CODEX_MONITOR_WINDOWS_VERSION `"$expectedPreviousVersion`"" `
        -Description "resource string version"
    $componentNames = @("MAJOR", "MINOR", "PATCH")
    for ($index = 0; $index -lt $componentNames.Count; $index++) {
        $oldResource = Replace-ExactlyOnce -Text $oldResource `
            -Pattern ("#define CODEX_MONITOR_WINDOWS_VERSION_" +
                $componentNames[$index] + " [0-9]+") `
            -Replacement ("#define CODEX_MONITOR_WINDOWS_VERSION_" +
                $componentNames[$index] + " " + $oldVersionParts[$index]) `
            -Description ("resource " + $componentNames[$index] + " version")
    }
    [IO.File]::WriteAllText($oldResourcePath, $oldResource,
        [Text.UTF8Encoding]::new($false))

    & cmake -S $oldWindows -B $oldBuild -A x64 -DBUILD_TESTING=OFF
    if ($LASTEXITCODE -ne 0) {
        throw "Could not configure the previous Windows build"
    }
    & cmake --build $oldBuild --config Release --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Could not compile the previous Windows build"
    }
    $oldBinary = (Resolve-Path -LiteralPath (
        Join-Path $oldBuild "Release/CodexMonitorHUD.exe")).Path
    $oldVersionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($oldBinary)
    if ($oldVersionInfo.FileVersion -ne $expectedPreviousVersion -or
        $oldVersionInfo.ProductVersion -ne $expectedPreviousVersion) {
        throw "Previous EXE version metadata is invalid: file=$($oldVersionInfo.FileVersion) product=$($oldVersionInfo.ProductVersion)"
    }
    & (Join-Path $oldWindows "build-installer.ps1") `
        -BinaryPath $oldBinary -OutputDirectory $oldInstallerOutput
    if ($LASTEXITCODE -ne 0) {
        throw "Could not build the previous MSI"
    }
    $oldInstallers = @(Get-ChildItem -LiteralPath $oldInstallerOutput `
        -Filter "CodexMonitorHUD-windows-x64-*.msi" -File)
    if ($oldInstallers.Count -ne 1) {
        throw "Expected one previous MSI, found $($oldInstallers.Count)"
    }
    $oldInstaller = $oldInstallers[0].FullName
    Write-Host "phase=previous-build done"

    Write-Host "phase=identity start"
    $oldVersion = Get-MsiProperty -InstallerPath $oldInstaller `
        -PropertyName "ProductVersion"
    $oldProductCode = ConvertTo-CanonicalGuid `
        -Value (Get-MsiProperty -InstallerPath $oldInstaller `
            -PropertyName "ProductCode") `
        -Description "Previous ProductCode"
    $oldUpgradeCode = ConvertTo-CanonicalGuid `
        -Value (Get-MsiProperty -InstallerPath $oldInstaller `
            -PropertyName "UpgradeCode") `
        -Description "Previous UpgradeCode"
    if ($oldVersion -ne $expectedPreviousVersion) {
        throw "Previous MSI version is $oldVersion, expected $expectedPreviousVersion"
    }
    if ($oldUpgradeCode -ne $expectedUpgradeCode -or
        $oldUpgradeCode -ne $currentUpgradeCode) {
        throw "Previous and current MSI UpgradeCodes do not match"
    }
    if ($oldProductCode -eq $currentProductCode) {
        throw "Major-upgrade MSI files must have different ProductCodes"
    }
    Write-Host "phase=identity done previous_product=$oldProductCode current_product=$currentProductCode"

    Write-Host "phase=previous-install start"
    $cleanupOldProduct = $true
    $oldInstallLog = Join-Path $logsDirectory "previous-install.log"
    $oldInstallStatus = Invoke-MsiExec -Operation install `
        -Target $oldInstaller -InstallFolder $installDirectory `
        -LogPath $oldInstallLog
    if ($oldInstallStatus -notin $successExitCodes) {
        throw "Previous MSI installation failed with exit code $oldInstallStatus"
    }
    if (-not (Test-Path -LiteralPath $installedBinary -PathType Leaf)) {
        throw "Previous MSI did not install the application executable"
    }
    $installedOldVersion = ([Diagnostics.FileVersionInfo]::GetVersionInfo(
        $installedBinary)).FileVersion
    if ($installedOldVersion -ne $expectedPreviousVersion) {
        throw "Installed previous EXE has version $installedOldVersion"
    }
    $oldInstalledHash = (Get-FileHash -LiteralPath $installedBinary `
        -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($preservedMarker, "preserve across upgrade`n",
        [Text.UTF8Encoding]::new($false))
    $registeredAfterOld = @(Get-RelatedProductCodes `
        -UpgradeCode $expectedUpgradeCode)
    if ($registeredAfterOld.Count -ne 1 -or
        $registeredAfterOld[0] -ne $oldProductCode) {
        throw "Previous MSI did not produce exactly one related product registration"
    }
    Write-Host "phase=previous-install done"

    Write-Host "phase=major-upgrade start"
    $cleanupCurrentProduct = $true
    $upgradeLog = Join-Path $logsDirectory "major-upgrade.log"
    $upgradeStatus = Invoke-MsiExec -Operation install `
        -Target $resolvedCurrentInstaller -LogPath $upgradeLog
    if ($upgradeStatus -notin $successExitCodes) {
        throw "Current MSI upgrade failed with exit code $upgradeStatus"
    }
    if (-not (Test-Path -LiteralPath $installedBinary -PathType Leaf)) {
        throw "Upgrade did not preserve the customized installation directory"
    }
    $currentInstalledHash = (Get-FileHash -LiteralPath $installedBinary `
        -Algorithm SHA256).Hash
    $sourceCurrentHash = (Get-FileHash -LiteralPath $resolvedCurrentBinary `
        -Algorithm SHA256).Hash
    if ([string]::IsNullOrWhiteSpace($expectedPublisher)) {
        if ($currentInstalledHash -ne $sourceCurrentHash) {
            throw "Upgraded executable does not match the current tested build"
        }
    } else {
        $installedPublisher = Get-TrustedPublisherSha256 -Path $installedBinary
        if ($installedPublisher -ne $expectedPublisher) {
            throw "The upgraded executable is not signed by the expected publisher"
        }
    }
    $expectedInstalledHash = $currentInstalledHash
    if ($currentInstalledHash -eq $oldInstalledHash) {
        throw "Upgrade did not replace the previous executable"
    }
    $installedCurrentVersion = ([Diagnostics.FileVersionInfo]::GetVersionInfo(
        $installedBinary)).FileVersion
    if ($installedCurrentVersion -ne $currentVersion) {
        throw "Upgraded EXE version is $installedCurrentVersion, expected $currentVersion"
    }
    if (-not (Test-Path -LiteralPath $preservedMarker -PathType Leaf)) {
        throw "Major upgrade removed a user-created file from the installation directory"
    }
    $recordedInstallFolder = [string](Get-ItemPropertyValue `
        -LiteralPath "HKCU:\Software\CodexMonitorHUD" `
        -Name "InstallFolder")
    if ((Get-NormalizedPath -Path $recordedInstallFolder) -ine
        (Get-NormalizedPath -Path $installDirectory)) {
        throw "Upgrade changed the customized installation directory"
    }
    $registeredAfterUpgrade = @(Get-RelatedProductCodes `
        -UpgradeCode $expectedUpgradeCode)
    if ($registeredAfterUpgrade.Count -ne 1 -or
        $registeredAfterUpgrade[0] -ne $currentProductCode) {
        throw "Major upgrade did not leave exactly the current product registered"
    }
    Write-Host "phase=major-upgrade done"

    Write-Host "phase=downgrade-rejection start"
    $downgradeLog = Join-Path $logsDirectory "downgrade-rejection.log"
    $downgradeStatus = Invoke-MsiExec -Operation install `
        -Target $oldInstaller -LogPath $downgradeLog
    if ($downgradeStatus -in $successExitCodes) {
        throw "Previous MSI unexpectedly downgraded the current installation"
    }
    $downgradeLogText = Get-Content -LiteralPath $downgradeLog -Raw
    if ($downgradeLogText -notmatch "WIX_DOWNGRADE_DETECTED" -or
        $downgradeLogText -notmatch [regex]::Escape($expectedDowngradeMessage)) {
        throw "Previous MSI failed for a reason other than the expected downgrade gate"
    }
    if (-not (Test-Path -LiteralPath $installedBinary -PathType Leaf)) {
        throw "Rejected downgrade removed the current executable"
    }
    $hashAfterDowngrade = (Get-FileHash -LiteralPath $installedBinary `
        -Algorithm SHA256).Hash
    if ($hashAfterDowngrade -ne $expectedInstalledHash) {
        throw "Rejected downgrade changed the current executable"
    }
    $registeredAfterDowngrade = @(Get-RelatedProductCodes `
        -UpgradeCode $expectedUpgradeCode)
    if ($registeredAfterDowngrade.Count -ne 1 -or
        $registeredAfterDowngrade[0] -ne $currentProductCode) {
        throw "Rejected downgrade changed related product registration"
    }
    Write-Host "phase=downgrade-rejection done exit_code=$downgradeStatus"
    Write-Host "msi_major_upgrade_test=pass previous=$expectedPreviousVersion current=$currentVersion"
} catch {
    $primaryFailure = $_
    Write-Error -ErrorRecord $_ -ErrorAction Continue
    Show-MsiLogs -Directory $logsDirectory
} finally {
    Write-Host "phase=cleanup start"
    if ($cleanupCurrentProduct -and
        -not [string]::IsNullOrWhiteSpace($currentProductCode)) {
        try {
            $status = Invoke-MsiExec -Operation uninstall `
                -Target $currentProductCode `
                -LogPath (Join-Path $logsDirectory "current-uninstall.log")
            if ($status -notin ($successExitCodes + $absentProductExitCodes)) {
                $cleanupFailures.Add(
                    "Current product uninstall returned $status")
            }
        } catch {
            $cleanupFailures.Add(
                "Current product uninstall failed: $($_.Exception.Message)")
        }
    }
    if ($cleanupOldProduct -and
        -not [string]::IsNullOrWhiteSpace($oldProductCode)) {
        try {
            $status = Invoke-MsiExec -Operation uninstall `
                -Target $oldProductCode `
                -LogPath (Join-Path $logsDirectory "previous-uninstall.log")
            if ($status -notin ($successExitCodes + $absentProductExitCodes)) {
                $cleanupFailures.Add(
                    "Previous product uninstall returned $status")
            }
        } catch {
            $cleanupFailures.Add(
                "Previous product uninstall failed: $($_.Exception.Message)")
        }
    }

    if ($cleanupFailures.Count -eq 0 -and $verifyNoProductsAfterCleanup) {
        try {
            $remainingProducts = @(Get-RelatedProductCodes `
                -UpgradeCode $expectedUpgradeCode)
            if ($remainingProducts.Count -ne 0) {
                $cleanupFailures.Add(
                    "Related products remain registered after cleanup")
            }
        } catch {
            $cleanupFailures.Add(
                "Could not verify product cleanup: $($_.Exception.Message)")
        }
    }

    if ($cleanupFailures.Count -eq 0 -and
        (Test-Path -LiteralPath $testRoot -PathType Container)) {
        try {
            Remove-Item -LiteralPath $testRoot -Recurse -Force
        } catch {
            $cleanupFailures.Add(
                "Could not remove the test directory: $($_.Exception.Message)")
        }
    }
    if ($cleanupFailures.Count -eq 0) {
        Write-Host "phase=cleanup done"
    } else {
        Show-MsiLogs -Directory $logsDirectory
        foreach ($failure in $cleanupFailures) {
            Write-Warning $failure
        }
    }
}

if ($null -ne $primaryFailure) {
    throw $primaryFailure
}
if ($cleanupFailures.Count -ne 0) {
    throw "MSI major-upgrade test cleanup was incomplete"
}
