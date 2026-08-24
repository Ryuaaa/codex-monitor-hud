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
schema_version: 1
record_type: task
record_status: current
title: Windows runner lifecycle fixture
task_status: doing
domain: task_center_ci
priority: medium
privacy: general
codex_access: proposal_only
source_refs: ["synthetic-ci"]
verification_status: synthetic_fixture
related_ids: []
---
Synthetic CI body. This is not formal task data.
"@ | Set-Content -Encoding utf8 -Path (Join-Path $taskRoot "tsk_windows_ci.md")
$env:CODEX_TASK_CENTER_TASK_ROOT = $taskRoot

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class TaskCenterNativeWindow {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr hWnd, StringBuilder text, int count);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PostMessage(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);
}
"@

function Get-ProcessIds([string]$Name) {
    return @(
        Get-Process -Name $Name -ErrorAction SilentlyContinue |
            ForEach-Object { $_.Id }
    )
}

function Get-NewProcessIds([string]$Name, [int[]]$Baseline) {
    return @(Get-ProcessIds $Name | Where-Object { $Baseline -notcontains $_ })
}

function Get-ProcessWindows([int]$ProcessId) {
    $windows = [System.Collections.ArrayList]::new()
    $callback = [TaskCenterNativeWindow+EnumWindowsProc]{
        param([IntPtr]$handle, [IntPtr]$unused)
        $owner = [uint32]0
        [TaskCenterNativeWindow]::GetWindowThreadProcessId($handle, [ref]$owner) | Out-Null
        if ($owner -eq $ProcessId) {
            $title = [System.Text.StringBuilder]::new(512)
            $className = [System.Text.StringBuilder]::new(256)
            [TaskCenterNativeWindow]::GetWindowText($handle, $title, $title.Capacity) | Out-Null
            [TaskCenterNativeWindow]::GetClassName($handle, $className, $className.Capacity) | Out-Null
            $windows.Add([ordered]@{
                handle = $handle.ToInt64()
                title = $title.ToString()
                className = $className.ToString()
                visible = [TaskCenterNativeWindow]::IsWindowVisible($handle)
            }) | Out-Null
        }
        return $true
    }
    [TaskCenterNativeWindow]::EnumWindows($callback, [IntPtr]::Zero) | Out-Null
    return @($windows)
}

function Wait-ForWindow([System.Diagnostics.Process]$Process, [int]$TimeoutSeconds = 30) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "Task Center exited before its native window became available (exit $($Process.ExitCode))."
        }
        $windows = @(Get-ProcessWindows $Process.Id)
        $target = @($windows | Where-Object { $_.visible -and $_.title -eq "Codex Monitor 任务中心" })
        if ($target.Count -eq 1) {
            return $target[0]
        }
        if ($target.Count -gt 1) {
            throw "Task Center exposed more than one visible main window."
        }
        Start-Sleep -Milliseconds 250
    }
    $observed = @(Get-ProcessWindows $Process.Id | ConvertTo-Json -Compress)
    throw "Task Center did not expose the expected visible titled window within $TimeoutSeconds seconds. Observed: $observed"
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

function Get-BoundEndpoints([int[]]$ProcessIds) {
    if ($ProcessIds.Count -eq 0) {
        return @()
    }
    return @(
        Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
            Where-Object { $ProcessIds -contains $_.OwningProcess } |
            ForEach-Object {
                [ordered]@{
                    protocol = "TCP"
                    address = $_.LocalAddress
                    port = $_.LocalPort
                    pid = $_.OwningProcess
                }
            }
        Get-NetUDPEndpoint -ErrorAction SilentlyContinue |
            Where-Object { $ProcessIds -contains $_.OwningProcess } |
            ForEach-Object {
                [ordered]@{
                    protocol = "UDP"
                    address = $_.LocalAddress
                    port = $_.LocalPort
                    pid = $_.OwningProcess
                }
            }
    )
}

function Get-TaskCenterServices {
    return @(
        Get-CimInstance Win32_Service -ErrorAction SilentlyContinue |
            Where-Object { $_.PathName -match "(?i)codex-monitor-task-center" } |
            ForEach-Object {
                [ordered]@{
                    name = $_.Name
                    state = $_.State
                    startMode = $_.StartMode
                    pathName = $_.PathName
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
$baselineServices = @(Get-TaskCenterServices)
$runtime = Get-WebView2RuntimeVersion
$cycleReports = @()

try {
    for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
        $webViewBefore = @(Get-ProcessIds "msedgewebview2")
        $process = Start-Process -FilePath $executablePath -PassThru
        $window = Wait-ForWindow $process
        $webViewIds = @(Wait-ForNewWebView $webViewBefore)
        $boundEndpoints = @(Get-BoundEndpoints (@($process.Id) + $webViewIds))
        if ($boundEndpoints.Count -ne 0) {
            throw "Cycle $cycle created a bound TCP or UDP endpoint."
        }
        if (-not [TaskCenterNativeWindow]::PostMessage([IntPtr]$window.handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)) {
            throw "Cycle $cycle could not post WM_CLOSE to the verified Task Center window."
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
            windowHandle = $window.handle
            windowTitle = $window.title
            windowClass = $window.className
            closeRequestAccepted = $true
            exitCode = $process.ExitCode
            webView2ProcessCount = $webViewIds.Count
            boundEndpointCount = $boundEndpoints.Count
            residualWebView2ProcessCount = $remainingWebView.Count
        }
    }

    $forceWebViewBefore = @(Get-ProcessIds "msedgewebview2")
    $sentinel = Start-Process -FilePath "powershell.exe" -ArgumentList "-NoProfile", "-Command", "Start-Sleep -Seconds 60" -PassThru
    $forced = Start-Process -FilePath $executablePath -PassThru
    $forcedWindow = Wait-ForWindow $forced
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
    $servicesAfter = @(Get-TaskCenterServices)
    if ($servicesAfter.Count -ne $baselineServices.Count) {
        throw "Task Center service inventory changed during lifecycle validation."
    }
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
        webView2Execution = [ordered]@{
            observedInEveryCycle = ($cycleReports.Count -eq $Cycles -and @($cycleReports | Where-Object { $_.webView2ProcessCount -lt 1 }).Count -eq 0)
            registryVersionAvailable = [bool]$runtime.version
            limitation = "Process creation proves the installed WebView2 runtime can execute; a missing registry value does not identify its installed version."
        }
        lifecycle = [ordered]@{
            requestedCycles = $Cycles
            passedCycles = $cycleReports.Count
            nativeCloseMethod = "PostMessage WM_CLOSE to the visible titled top-level Task Center HWND"
            cycles = $cycleReports
        }
        network = [ordered]@{
            taskCenterOrWebViewBoundTcpOrUdpEndpoints = 0
        }
        residualProcesses = [ordered]@{
            taskCenter = $taskCenterNamedResidual.Count
            newWebView2 = $newWebViewResidual.Count
            baselineNodeProcessIds = $baselineNode
            finalNodeProcessIds = $nodeAfter
            nodeSpawnedByTaskCenterObserved = $false
        }
        services = [ordered]@{
            baselineMatchingServices = $baselineServices
            finalMatchingServices = $servicesAfter
            serviceInventoryChanged = $false
        }
        forceTermination = [ordered]@{
            windowHandle = $forcedWindow.handle
            windowTitle = $forcedWindow.title
            windowClass = $forcedWindow.className
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
    [System.IO.File]::WriteAllText(
        (Join-Path $outputPath "SHA256SUMS.txt"),
        "$hash  $([System.IO.Path]::GetFileName($executablePath))`n",
        [System.Text.Encoding]::ASCII
    )
    Get-ChildItem -File $outputPath | Sort-Object Name | Select-Object Name, Length |
        ConvertTo-Json | Set-Content -Encoding utf8 -Path (Join-Path $outputPath "artifact-files.json")
    Write-Host "Windows lifecycle validation passed: $Cycles native close cycles, force termination, no listeners or residual Task Center/WebView2 processes."
}
finally {
    Get-Process -Name "codex-monitor-task-center" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}
