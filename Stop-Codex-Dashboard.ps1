param(
    [string]$InstallPath = (Join-Path $env:USERPROFILE '.codex\dashboard'),
    [string]$Profile = (Join-Path $env:APPDATA 'Codex\web\Codex'),
    [int]$CdpPort = 0
)
$ErrorActionPreference = 'Stop'
$sidecar = Join-Path $PSScriptRoot 'codex-dashboard.exe'
if (-not (Test-Path -LiteralPath $sidecar)) { $sidecar = Join-Path $InstallPath 'codex-dashboard.exe' }
$ownStart = Join-Path (Split-Path -Parent $sidecar) 'Start-Codex-Dashboard.ps1'
Get-CimInstance Win32_Process -Filter "Name='pwsh.exe' OR Name='powershell.exe'" | Where-Object {
    $_.CommandLine -and $_.CommandLine.Contains($ownStart) -and $_.CommandLine -match '(?i)\s-Watchdog(\s|$)'
} | ForEach-Object { Stop-Process -Id $_.ProcessId -ErrorAction Stop }
$owned = @(Get-CimInstance Win32_Process -Filter "Name='codex-dashboard.exe'" | Where-Object { $_.ExecutablePath -eq $sidecar })
if ($CdpPort) { $ports = @($CdpPort) } else {
    $ports = @($owned | ForEach-Object { if ($_.CommandLine -match '--cdp-port\s+(\d+)') { [int]$Matches[1] } } | Select-Object -Unique)
}
foreach ($port in $ports) { & $sidecar --stop --cdp-port $port }
foreach ($entry in $owned) {
    if ($CdpPort -and $entry.CommandLine -notmatch "--cdp-port\s+$CdpPort(\s|$)") { continue }
    $proc = Get-Process -Id $entry.ProcessId -ErrorAction SilentlyContinue
    if ($proc -and -not $proc.WaitForExit(10000)) { throw '采集器未及时退出，请保留现场检查。' }
}
Write-Host '本安装的采集器已停止；未结束 Codex 客户端。'
