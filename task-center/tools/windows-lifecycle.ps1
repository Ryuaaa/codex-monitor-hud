param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [int]$Cycles = 20
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$executablePath = (Resolve-Path $Executable).Path
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null

$fixtureRoot = Join-Path $env:RUNNER_TEMP "codex-task-center-fixture"
$taskRoot = Join-Path $fixtureRoot "任务"
$eventsRoot = Join-Path $fixtureRoot "事件"
New-Item -ItemType Directory -Force -Path $taskRoot, $eventsRoot | Out-Null
@"
---
task_id: tsk_windows_ci
title: Windows runner lifecycle fixture
status: doing
priority: normal
privacy: general
codex_access: proposal_only
---
Synthetic CI body. This is not formal task data.
"@ | Set-Content -Encoding utf8 -Path (Join-Path $taskRoot "tsk_windows_ci.md")
$env:CODEX_TASK_CENTER_TASK_ROOT = $taskRoot

function Get-ProcessIds([string]$Name) {
    return @(
        Get-Process -Name $Name -ErrorAction SilentlyContinue |
            ForEach-Object { $_.Id }
    )
}

function Get-NewProcessIds([string]$Name, [int[]]$Baseline) {
    return @(Get-ProcessIds $Name | Where-Object { $Baseline -notcontains $_ })
}

function Wait-ForWindow([System.Diagnostics.Process]$Process, [int]$TimeoutSeconds = 30) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "Task Center exited before its native window became available (exit $($Process.ExitCode))."
        }
        if ($Process.MainWindowHandle -ne [IntPtr]::Zero) {
            return $Process.MainWindowHandle.ToInt64()
        }
        Start-Sleep -Milliseconds 250
    }
    throw "Task Center did not expose a native window handle within $TimeoutSeconds seconds."
}

function Wait-ForNewWebView([int[]]$Baseline, [int]$TimeoutSeconds = 30) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $ids = @(Get-NewProcessIds "msedgewebview2" $Baseline)
        if ($ids.Count -gt 0) {
            return $ids
        }
        Start-Sleep -Milliseconds 250
    }
    throw "No new WebView2 process appeared within $TimeoutSeconds seconds."
}

function Wait-ForExit([System.Diagnostics.Process]$Process, [int]$TimeoutSeconds = 10) {
    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        return $false
    }
    return $true
}

function Wait-ForProcessesGone([int[]]$ProcessIds, [int]$TimeoutSeconds = 15) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $remaining = @($ProcessIds | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
        if ($remaining.Count -eq 0) {
            return @()
        }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    return $remaining
}

function Get-ListeningEndpoints([int[]]$ProcessIds) {
    if ($ProcessIds.Count -eq 0) {
        return @()
    }
    return @(
        Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
            Where-Object { $ProcessIds -contains $_.OwningProcess } |
            ForEach-Object {
                [ordered]@{
                    address = $_.LocalAddress
                    port = $_.LocalPort
                    pid = $_.OwningProcess
                }
            }
    )
}

function Get-WebView2RuntimeVersion {
    $clientId = "{F3017226-FE2A-4295-8BDF-00C72E782AB6}"
    $keys = @(
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\$clientId",
        "HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$clientId",
        "HKCU:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$clientId"
    )
    foreach ($key in $keys) {
        if (Test-Path $key) {
            $version = (Get-ItemProperty -Path $key -Name pv -ErrorAction SilentlyContinue).pv
            if ($version) {
                return [ordered]@{ version = $version; registryKey = $key }
            }
        }
    }
    return [ordered]@{ version = $null; registryKey = $null }
}

$baselineWebView = @(Get-ProcessIds "msedgewebview2")
$baselineNode = @(Get-ProcessIds "node")
$runtime = Get-WebView2RuntimeVersion
$cycleReports = @()

try {
    for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
        $webViewBefore = @(Get-ProcessIds "msedgewebview2")
        $process = Start-Process -FilePath $executablePath -PassThru
        $windowHandle = Wait-ForWindow $process
        $webViewIds = @(Wait-ForNewWebView $webViewBefore)
        $listeners = @(Get-ListeningEndpoints (@($process.Id) + $webViewIds))
        if ($listeners.Count -ne 0) {
            throw "Cycle $cycle created a listening TCP endpoint."
        }
        if (-not $process.CloseMainWindow()) {
            throw "Cycle $cycle could not send the native close-window request."
        }
        if (-not (Wait-ForExit $process)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            throw "Cycle $cycle did not exit within 10 seconds after native close."
        }
        $remainingWebView = @(Wait-ForProcessesGone $webViewIds)
        if ($remainingWebView.Count -ne 0) {
            throw "Cycle $cycle left WebView2 process IDs: $($remainingWebView -join ', ')."
        }
        $cycleReports += [ordered]@{
            cycle = $cycle
            windowHandle = $windowHandle
            closeRequestAccepted = $true
            exitCode = $process.ExitCode
            webView2ProcessCount = $webViewIds.Count
            listeningEndpointCount = $listeners.Count
            residualWebView2ProcessCount = $remainingWebView.Count
        }
    }

    $forceWebViewBefore = @(Get-ProcessIds "msedgewebview2")
    $sentinel = Start-Process -FilePath "powershell.exe" -ArgumentList "-NoProfile", "-Command", "Start-Sleep -Seconds 60" -PassThru
    $forced = Start-Process -FilePath $executablePath -PassThru
    $forcedHandle = Wait-ForWindow $forced
    $forcedWebViewIds = @(Wait-ForNewWebView $forceWebViewBefore)
    Stop-Process -Id $forced.Id -Force
    if (-not (Wait-ForExit $forced)) {
        throw "Force-terminated Task Center process did not exit."
    }
    $forcedResidual = @(Wait-ForProcessesGone $forcedWebViewIds)
    $sentinel.Refresh()
    $sentinelSurvived = -not $sentinel.HasExited
    Stop-Process -Id $sentinel.Id -Force -ErrorAction SilentlyContinue
    if ($forcedResidual.Count -ne 0) {
        throw "Force termination left WebView2 process IDs: $($forcedResidual -join ', ')."
    }
    if (-not $sentinelSurvived) {
        throw "Independent sentinel process did not survive Task Center termination."
    }

    $taskCenterNamedResidual = @(Get-ProcessIds "codex-monitor-task-center")
    $newWebViewResidual = @(Get-NewProcessIds "msedgewebview2" $baselineWebView)
    if ($taskCenterNamedResidual.Count -ne 0 -or $newWebViewResidual.Count -ne 0) {
        throw "Task Center or new WebView2 processes remain after lifecycle checks."
    }

    $nodeAfter = @(Get-ProcessIds "node")
    $fileVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($executablePath)
    $hash = (Get-FileHash -Algorithm SHA256 -Path $executablePath).Hash.ToLowerInvariant()
    $report = [ordered]@{
        schemaVersion = 1
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
        runner = [ordered]@{
            os = [System.Environment]::OSVersion.VersionString
            architecture = $env:PROCESSOR_ARCHITECTURE
            image = $env:ImageOS
            imageVersion = $env:ImageVersion
        }
        executable = [ordered]@{
            name = [System.IO.Path]::GetFileName($executablePath)
            fileVersion = $fileVersion.FileVersion
            productVersion = $fileVersion.ProductVersion
            bytes = (Get-Item $executablePath).Length
            sha256 = $hash
        }
        webView2Runtime = $runtime
        lifecycle = [ordered]@{
            requestedCycles = $Cycles
            passedCycles = $cycleReports.Count
            nativeCloseMethod = "Process.CloseMainWindow (WM_CLOSE)"
            cycles = $cycleReports
        }
        network = [ordered]@{
            taskCenterOrWebViewListeningEndpoints = 0
        }
        residualProcesses = [ordered]@{
            taskCenter = $taskCenterNamedResidual.Count
            newWebView2 = $newWebViewResidual.Count
            baselineNodeProcessIds = $baselineNode
            finalNodeProcessIds = $nodeAfter
            nodeSpawnedByTaskCenterObserved = $false
        }
        forceTermination = [ordered]@{
            windowHandle = $forcedHandle
            taskCenterExited = $true
            residualWebView2 = $forcedResidual.Count
            independentSentinelSurvived = $sentinelSurvived
            limitation = "Sentinel proves process isolation only; it is not a Windows HUD runtime test."
        }
        evidenceBoundary = [ordered]@{
            interactiveDesktopExperienceVerified = $false
            systemScalingVerified = $false
            energyAndWakeupsVerified = $false
        }
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 -Path (Join-Path $outputPath "windows-lifecycle-report.json")
    Copy-Item -Force $executablePath (Join-Path $outputPath ([System.IO.Path]::GetFileName($executablePath)))
    "$hash  $([System.IO.Path]::GetFileName($executablePath))" | Set-Content -Encoding ascii -Path (Join-Path $outputPath "SHA256SUMS.txt")
    Get-ChildItem -File $outputPath | Sort-Object Name | Select-Object Name, Length |
        ConvertTo-Json | Set-Content -Encoding utf8 -Path (Join-Path $outputPath "artifact-files.json")
    Write-Host "Windows lifecycle validation passed: $Cycles native close cycles, force termination, no listeners or residual Task Center/WebView2 processes."
}
finally {
    Get-Process -Name "codex-monitor-task-center" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}
