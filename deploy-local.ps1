# deploy-local.ps1 — 一键本地部署（自动提权 + 日志 + 防闪退）
# 注意：本文件必须为 UTF-8 with BOM（PowerShell 5.1 无 BOM 会按 GBK 解码导致语法崩）
# 推荐入口: 双击 deploy-local.bat（窗口常驻、可看错误）
param(
    [string]$BuildDll = (Join-Path $PSScriptRoot "build-release\AIUsagePreview.dll"),
    [string]$TrafficMonitorDir = (Join-Path (Split-Path $PSScriptRoot -Parent) "TrafficMonitor")
)

$ErrorActionPreference = "Stop"

function Pause-IfInteractive {
    try {
        if ([Environment]::UserInteractive -and -not [Console]::IsInputRedirected) {
            Read-Host "`nPress Enter to close..."
        }
    } catch {}
}

try {
    # 1) 自我提权：非管理员则弹 UAC 以管理员重启自身（.bat 已提权时此处直接跳过）
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        Write-Host "[*] Not admin, requesting elevation (UAC)..." -ForegroundColor Yellow
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = "powershell.exe"
        $psi.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -BuildDll `"$BuildDll`" -TrafficMonitorDir `"$TrafficMonitorDir`""
        $psi.Verb = "RunAs"
        try {
            [System.Diagnostics.Process]::Start($psi) | Out-Null
        } catch {
            Write-Error "Elevation cancelled or failed: $_"
            Write-Host "Please open a terminal AS ADMINISTRATOR and run: .\deploy-local.ps1" -ForegroundColor Red
            throw
        }
        Write-Host "[*] Elevated instance launched in a new window." -ForegroundColor Green
        exit 0
    }

    # Admin branch: start transcript log
    $logFile = Join-Path $PSScriptRoot "deploy-local.log"
    try { Start-Transcript -Path $logFile -Append -ErrorAction SilentlyContinue } catch {}
    Write-Host "[*] Running as administrator. Log: $logFile" -ForegroundColor Green

    # 1b) Pre-flight: validate inputs BEFORE touching any process, so a
    #     missing DLL/dir can never leave TrafficMonitor killed with nothing
    #     to deploy.
    if (!(Test-Path -LiteralPath $TrafficMonitorDir -PathType Container)) {
        throw "TrafficMonitor dir not found: $TrafficMonitorDir (pass -TrafficMonitorDir)"
    }
    if (!(Test-Path -LiteralPath $BuildDll -PathType Leaf)) {
        throw "Build DLL not found: $BuildDll (run: ninja -C build-release AIUsage)"
    }
    $pluginsDir = Join-Path $TrafficMonitorDir "plugins"
    if (!(Test-Path -LiteralPath $pluginsDir -PathType Container)) {
        throw "plugins dir not found: $pluginsDir"
    }
    $tmExe = Join-Path $TrafficMonitorDir "TrafficMonitor.exe"
    if (!(Test-Path -LiteralPath $tmExe -PathType Leaf)) {
        throw "TrafficMonitor.exe not found: $tmExe"
    }
    Write-Host "    [OK] Pre-flight: DLL, plugins dir, TrafficMonitor.exe all present." -ForegroundColor Green

    # 2) Kill old TrafficMonitor (force; can kill elevated instance and stray threads)
    Write-Host "[*] Stopping old TrafficMonitor..." -ForegroundColor Yellow
    $deadline = (Get-Date).AddSeconds(15)
    do {
        $remaining = @(Get-Process -Name "TrafficMonitor" -ErrorAction SilentlyContinue)
        if ($remaining.Count -gt 0) { $remaining | Stop-Process -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 300
    } while (@(Get-Process -Name "TrafficMonitor" -ErrorAction SilentlyContinue).Count -gt 0 `
        -and (Get-Date) -lt $deadline)

    $check = @(Get-Process -Name "TrafficMonitor" -ErrorAction SilentlyContinue)
    if ($check.Count -ne 0) {
        throw "Residual TrafficMonitor processes: $($check.Id -join ', '). Kill them in an elevated terminal and retry."
    }
    Write-Host "    [OK] Old processes all stopped." -ForegroundColor Green

    # 3) Overwrite DLL (retry briefly: a just-exited process may still be
    #    releasing its file lock; verify the size matches before continuing)
    $dstDll = Join-Path $TrafficMonitorDir "plugins\AIUsagePreview.dll"
    Write-Host "[*] Deploying DLL..." -ForegroundColor Yellow
    $srcLen = (Get-Item -LiteralPath $BuildDll).Length
    $copied = $false
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            Copy-Item -LiteralPath $BuildDll -Destination $dstDll -Force -ErrorAction Stop
            $dstLen = (Get-Item -LiteralPath $dstDll -ErrorAction Stop).Length
            if ($dstLen -eq $srcLen) { $copied = $true; break }
            Write-Host "    [i] Copy attempt $attempt : size mismatch ($dstLen vs $srcLen), retrying..." -ForegroundColor Yellow
        } catch {
            Write-Host "    [i] Copy attempt $attempt failed: $_" -ForegroundColor Yellow
            Start-Sleep -Milliseconds 400
        }
    }
    if (-not $copied) { throw "Failed to deploy DLL after 3 attempts: $dstDll" }
    $deployedLen = (Get-Item -LiteralPath $dstDll).Length
    Write-Host "    [OK] $dstDll ($deployedLen bytes)" -ForegroundColor Green
    Write-Host "    [i] Local AIUsage.ini preserved." -ForegroundColor Gray

    # 3b) Deploy ag-login.exe (one-time Google authorizer for Antigravity quota)
    $srcAgLogin = Join-Path $PSScriptRoot "tools\ag-login\ag-login.exe"
    if (Test-Path $srcAgLogin) {
        Copy-Item $srcAgLogin (Join-Path $TrafficMonitorDir "ag-login.exe") -Force
        Write-Host "    [OK] ag-login.exe 已部署（双击一次即可授权 Antigravity，令牌自动写回 ini）" -ForegroundColor Green
    } else {
        Write-Host "    [i] ag-login.exe 未找到，跳过（Antigravity 仍可手动填 ini 授权）" -ForegroundColor Gray
    }

    # 4) Restart (portable app: working dir must be its own folder so config
    #    and plugins resolve the same as a manual double-click)
    Write-Host "[*] Starting TrafficMonitor..." -ForegroundColor Yellow
    Start-Process -FilePath $tmExe -WorkingDirectory $TrafficMonitorDir
    $tm = @()
    $startDeadline = (Get-Date).AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 500
        $tm = @(Get-Process -Name "TrafficMonitor" -ErrorAction SilentlyContinue)
    } while ($tm.Count -lt 1 -and (Get-Date) -lt $startDeadline)
    if ($tm.Count -ne 1) { throw "Expected exactly 1 instance, found $($tm.Count)." }
    Write-Host "    [OK] PID=$($tm[0].Id), instance count=1" -ForegroundColor Green
    Write-Host "`n[OK] Deploy complete." -ForegroundColor Cyan
}
catch {
    Write-Error $_
    Write-Host "`n[FAILED] Deploy failed. Log: $(Join-Path $PSScriptRoot 'deploy-local.log')" -ForegroundColor Red
}
finally {
    try { Stop-Transcript -ErrorAction SilentlyContinue } catch {}
    Pause-IfInteractive
}
