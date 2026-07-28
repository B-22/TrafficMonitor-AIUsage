# deploy-local.ps1 — 单实例本地部署脚本
# 用法: .\deploy-local.ps1

param(
    [string]$BuildDll = (Join-Path $PSScriptRoot "build-secure-check\AIUsagePreview.dll"),
    [string]$TrafficMonitorDir = (Join-Path (Split-Path $PSScriptRoot -Parent) "TrafficMonitor")
)

$ErrorActionPreference = "Stop"

$dll = $BuildDll
$dstDll = Join-Path $TrafficMonitorDir "plugins\AIUsagePreview.dll"
$tmExe = Join-Path $TrafficMonitorDir "TrafficMonitor.exe"

# 检查 DLL 是否存在
if (!(Test-Path $dll)) {
    Write-Host "ERROR: $dll not found. Build first." -ForegroundColor Red
    exit 1
}

# 1. 终止 TrafficMonitor
Write-Host "Stopping TrafficMonitor..." -ForegroundColor Yellow
$deadline = (Get-Date).AddSeconds(15)
do {
    $remaining = @(Get-Process -Name "TrafficMonitor" -ErrorAction SilentlyContinue)
    if ($remaining.Count -gt 0) {
        $remaining | Stop-Process -Force -ErrorAction Stop
    }
    Start-Sleep -Milliseconds 300
} while (@(Get-Process -Name "TrafficMonitor" -ErrorAction SilentlyContinue).Count -gt 0 `
    -and (Get-Date) -lt $deadline)

$check = @(Get-Process -Name "TrafficMonitor" -ErrorAction SilentlyContinue)
if ($check.Count -ne 0) {
    throw "TrafficMonitor residual processes: $($check.Id -join ', ')"
}
Write-Host "  All processes killed." -ForegroundColor Green

# 2. 复制文件
Write-Host "Deploying..." -ForegroundColor Yellow
Copy-Item $dll $dstDll -Force
Write-Host "  DLL: $dstDll ($((Get-Item $dstDll).Length) bytes)" -ForegroundColor Green
Write-Host "  Local AIUsage.ini preserved." -ForegroundColor Green

# 3. 启动 TrafficMonitor
Write-Host "Starting TrafficMonitor..." -ForegroundColor Yellow
Start-Process $tmExe
Start-Sleep -Seconds 2

$tm = @(Get-Process -Name "TrafficMonitor" -ErrorAction SilentlyContinue)
if ($tm.Count -ne 1) {
    throw "Expected exactly one TrafficMonitor process, found $($tm.Count)"
}
Write-Host "  OK: PID=$($tm[0].Id), instance count=1" -ForegroundColor Green

Write-Host "`nDeploy complete." -ForegroundColor Cyan
