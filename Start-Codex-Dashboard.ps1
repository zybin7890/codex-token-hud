param(
    [switch]$Watchdog,
    [switch]$Daemon,
    [switch]$Install,
    [string]$InstallPath = (Join-Path $env:USERPROFILE '.codex\dashboard'),
    [switch]$AttachOnly,
    [int]$CdpPort = 0,
    [string]$CodexAppPath = '',
    [string]$Profile = (Join-Path $env:APPDATA 'Codex\web\Codex'),
    [int]$IntervalSeconds = 3
)
$ErrorActionPreference = 'Stop'

function Get-CodexAppExecutable {
    param([string]$ExplicitPath)
    if ($ExplicitPath -and (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }
    $package = Get-AppxPackage -Name OpenAI.Codex | Select-Object -First 1
    if ($package) {
        $p1 = Join-Path $package.InstallLocation 'app\ChatGPT.exe'
        if (Test-Path -LiteralPath $p1 -PathType Leaf) { return $p1 }
        $p2 = Join-Path $package.InstallLocation 'app\Codex.exe'
        if (Test-Path -LiteralPath $p2 -PathType Leaf) { return $p2 }
    }
    $known = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\OpenAI.Codex\Codex.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\OpenAI.Codex\ChatGPT.exe'),
        (Join-Path $env:ProgramFiles 'OpenAI.Codex\Codex.exe')
    )
    foreach ($cand in $known) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) { return $cand }
    }
    return $null
}

function Get-RunningCodex {
    return Get-Process -Name ChatGPT, Codex -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -like '*OpenAI.Codex*'
    }
}

function Install-Dashboard {
    param([string]$Target)
    $launcher = Join-Path $PSScriptRoot 'codex-launcher.exe'
    if (-not (Test-Path -LiteralPath $launcher -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'codex-dashboard.exe') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'widget\inline.js') -PathType Leaf)) {
        throw '缺少启动器、采集器或微件，请使用完整便携包。'
    }
    $links = @(
        (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Codex.lnk'),
        (Join-Path ([Environment]::GetFolderPath('Programs')) 'Codex.lnk')
    )
    $shell = New-Object -ComObject WScript.Shell
    $targetLauncher = Join-Path $Target 'codex-launcher.exe'
    foreach ($path in $links) {
        if ((Test-Path -LiteralPath $path) -and $shell.CreateShortcut($path).TargetPath -ne $targetLauncher) {
            throw "已有其他 Codex 入口，未覆盖：$path"
        }
    }
    Write-Host "正在安装 Codex Dashboard 到 $Target ..."
    if (-not (Test-Path -LiteralPath $Target)) {
        New-Item -ItemType Directory -Path $Target -Force | Out-Null
    }
    $sourceDir = $PSScriptRoot
    $exe = Join-Path $sourceDir 'codex-dashboard.exe'
    if (Test-Path -LiteralPath $exe) {
        Copy-Item -LiteralPath $exe -Destination (Join-Path $Target 'codex-dashboard.exe') -Force
    }
    Copy-Item -LiteralPath $launcher -Destination $targetLauncher -Force
    $widgetDir = Join-Path $sourceDir 'widget'
    if (Test-Path -LiteralPath $widgetDir) {
        $targetWidget = Join-Path $Target 'widget'
        New-Item -ItemType Directory -Path $targetWidget -Force | Out-Null
        Get-ChildItem -LiteralPath $widgetDir -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $targetWidget $_.Name) -Force
        }
    }
    Copy-Item -LiteralPath (Join-Path $sourceDir 'Start-Codex-Dashboard.ps1') -Destination (Join-Path $Target 'Start-Codex-Dashboard.ps1') -Force
    Copy-Item -LiteralPath (Join-Path $sourceDir 'Stop-Codex-Dashboard.ps1') -Destination (Join-Path $Target 'Stop-Codex-Dashboard.ps1') -Force
    $readme = Join-Path $sourceDir 'README.md'
    if (Test-Path -LiteralPath $readme) {
        Copy-Item -LiteralPath $readme -Destination (Join-Path $Target 'README.md') -Force
    }

    $startCmd = "@echo off`r`npwsh -NoProfile -ExecutionPolicy Bypass -File `"%~dp0Start-Codex-Dashboard.ps1`" %*`r`n"
    $stopCmd = "@echo off`r`npwsh -NoProfile -ExecutionPolicy Bypass -File `"%~dp0Stop-Codex-Dashboard.ps1`" %*`r`n"
    Set-Content -LiteralPath (Join-Path $Target 'start.cmd') -Value $startCmd -Encoding utf8
    Set-Content -LiteralPath (Join-Path $Target 'stop.cmd') -Value $stopCmd -Encoding utf8

    $app = Get-CodexAppExecutable -ExplicitPath $CodexAppPath
    foreach ($path in $links) {
        $link = $shell.CreateShortcut($path)
        $link.TargetPath = $targetLauncher
        $link.WorkingDirectory = $Target
        $link.Description = 'Codex'
        if ($app) { $link.IconLocation = "$app,0" }
        $link.Save()
    }

    Write-Host "安装完成！安装路径：$Target"
    Write-Host '桌面与开始菜单的 Codex 入口会静默挂载状态栏；关闭后不会自动重开。'
    Write-Host "常用命令："
    Write-Host "  启动 / 挂载：     pwsh $Target\Start-Codex-Dashboard.ps1"
    Write-Host "  守护自动重启：    pwsh $Target\Start-Codex-Dashboard.ps1 -Watchdog"
    Write-Host "  后台静默守护：    pwsh $Target\Start-Codex-Dashboard.ps1 -Watchdog -Daemon"
    Write-Host "  停止并撤下组件：  pwsh $Target\Stop-Codex-Dashboard.ps1"
}

# 1. 处理安装指令
if ($Install) {
    Install-Dashboard -Target $InstallPath
    return
}

# 2. 定位 sidecar 路径
$sidecar = Join-Path $PSScriptRoot 'codex-dashboard.exe'
if (-not (Test-Path -LiteralPath $sidecar -PathType Leaf)) {
    if (Test-Path -LiteralPath (Join-Path $InstallPath 'codex-dashboard.exe')) {
        $sidecar = Join-Path $InstallPath 'codex-dashboard.exe'
    } else {
        throw '缺少 codex-dashboard.exe，请使用已编译的便携包或指定 -Install 安装。'
    }
}
if ($CdpPort -lt 0 -or $CdpPort -gt 65535) { throw 'CdpPort 必须为 0..65535。' }

# 3. 守护进程后台化 (-Daemon)
if ($Daemon -and $Watchdog) {
    $scriptPath = $MyInvocation.MyCommand.Path
    $pidFile = Join-Path $PSScriptRoot 'watchdog.pid'
    $daemonArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$scriptPath`"", '-Watchdog')
    if ($CdpPort -ne 0) { $daemonArgs += @('-CdpPort', "$CdpPort") }
    if ($CodexAppPath) { $daemonArgs += @('-CodexAppPath', "`"$CodexAppPath`"") }
    if ($Profile) { $daemonArgs += @('-Profile', "`"$Profile`"") }
    $proc = Start-Process -FilePath 'pwsh' -ArgumentList $daemonArgs -WindowStyle Hidden -PassThru
    Set-Content -LiteralPath $pidFile -Value $proc.Id -Encoding utf8
    Write-Host "[Watchdog] 后台守护进程已启动，PID=$($proc.Id)。"
    Write-Host "如需停止守护及仪表盘，请运行 Stop-Codex-Dashboard.ps1。"
    return
}

# 4. 辅助函数：拉起 Codex 并等待调试端口
function Get-LiveStatus {
    param([string]$ExePath, [string]$ProfDir, [int]$Port)
    $statusArgs = @('--status', '--profile', $ProfDir)
    if ($Port -ne 0) { $statusArgs += @('--cdp-port', "$Port") }
    $raw = & $ExePath @statusArgs 2>$null
    if ($LASTEXITCODE -eq 0) { return ($raw | ConvertFrom-Json) }
    return $null
}
function Ensure-CodexWithDebugPort {
    param([string]$AppExe, [string]$ProfDir)
    if (Get-LiveStatus $sidecar $ProfDir $CdpPort) { return }
    if (Get-RunningCodex) { throw '当前 Codex 未启用调试入口，请正常退出后通过此脚本重开。' }
    if ($AttachOnly) { throw 'AttachOnly 模式不会启动 Codex。' }
    if ($CodexAppPath) {
        Start-Process -FilePath $AppExe -ArgumentList @('--remote-debugging-port=0','--remote-debugging-address=127.0.0.1') | Out-Null
    } else {
        & $sidecar --launch
        if ($LASTEXITCODE -ne 0) { throw '注册的 Codex 桌面程序启动失败。' }
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(25)
    do {
        if (Get-LiveStatus $sidecar $ProfDir $CdpPort) { return }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Codex 已启动，但调试入口未就绪。'
}

# 5. 辅助函数：启动 / 确保 sidecar 运行
function Ensure-SidecarRunning {
    param([string]$ExePath, [string]$ProfDir, [int]$Port)
    $live = Get-LiveStatus $ExePath $ProfDir $Port
    if (-not $live) { throw '调试端口不可用，未挂载。' }
    $Port = [int]$live.port
    $owned = Get-CimInstance Win32_Process -Filter "Name='codex-dashboard.exe'" | Where-Object {
        $_.ExecutablePath -eq $ExePath -and $_.CommandLine -match "--cdp-port\s+$Port(\s|$)"
    }
    if (-not $owned) {
        $arguments = @('--attach','--profile',('"'+$ProfDir+'"'),'--cdp-port',"$Port")
        $proc = Start-Process -FilePath $ExePath -ArgumentList $arguments -WindowStyle Hidden -PassThru
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    do {
        $live = Get-LiveStatus $ExePath $ProfDir $Port
        if ($live -and @($live.widgets | Where-Object mounted).Count -gt 0) {
            Write-Host "已验证控件挂载，调试端口 $Port。"
            return $proc
        }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    Write-Host '采集器已启动；当前没有可挂载的输入框，等待打开聊天。'
    return $proc
}

# 6. 辅助函数：恢复未完成任务与提示
function Restore-UnfinishedTasks {
    Write-Host 'Codex 已重开；活动任务是否继续由客户端管理。'
}

# 7. Watchdog 守护监控主循环
if ($Watchdog) {
    $resolvedAppPath = Get-CodexAppExecutable -ExplicitPath $CodexAppPath
    $portFile = Join-Path $Profile 'DevToolsActivePort'
    Write-Host "========================================================"
    Write-Host " Codex Dashboard 守护进程 (Watchdog Supervisor) 已启动"
    Write-Host " 监控目标: Codex 桌面端与实时用量仪表盘"
    Write-Host " 策略: Codex 退出时自动重新拉起客户端与仪表盘"
    Write-Host " 轮询间隔: $IntervalSeconds 秒 | 按 Ctrl+C 或运行 Stop-Codex-Dashboard.ps1 退出"
    Write-Host "========================================================"

    # 首次启动检查
    Ensure-CodexWithDebugPort -AppExe $resolvedAppPath -ProfDir $Profile
    Ensure-SidecarRunning -ExePath $sidecar -ProfDir $Profile -Port $CdpPort
    Restore-UnfinishedTasks

    $lastSeenAlive = [DateTime]::UtcNow

    try {
        while ($true) {
            Start-Sleep -Seconds $IntervalSeconds
            $codexRunning = Get-RunningCodex

            if (-not $codexRunning) {
                Write-Host "[Watchdog] [$(Get-Date -Format 'HH:mm:ss')] 检测到 Codex 客户端已退出！准备自动拉起..."
                # 重新拉起 Codex
                Ensure-CodexWithDebugPort -AppExe $resolvedAppPath -ProfDir $Profile
                # 等待客户端稳定加载
                Start-Sleep -Seconds 2
                # 恢复未完成任务状态
                Restore-UnfinishedTasks
                # 重新挂载 sidecar
                Ensure-SidecarRunning -ExePath $sidecar -ProfDir $Profile -Port $CdpPort
                Write-Host "[Watchdog] [$(Get-Date -Format 'HH:mm:ss')] Codex 重启完成，已尝试重新挂载仪表盘。"
            } else {
                # Codex 正常运行中，确保 sidecar 也在运行
                $sidecarProcs = Get-Process -Name 'codex-dashboard' -ErrorAction SilentlyContinue
                if (-not $sidecarProcs) {
                    Write-Host "[Watchdog] 检测到仪表盘 sidecar 未运行，正在重新挂载..."
                    Ensure-SidecarRunning -ExePath $sidecar -ProfDir $Profile -Port $CdpPort
                }
            }
        }
    } finally {
        Write-Host "[Watchdog] 守护进程已停止。"
    }
    return
}

# 8. 单次启动 / 挂载模式 (默认)
Ensure-CodexWithDebugPort -AppExe (Get-CodexAppExecutable -ExplicitPath $CodexAppPath) -ProfDir $Profile
$proc = Ensure-SidecarRunning -ExePath $sidecar -ProfDir $Profile -Port $CdpPort
Write-Host '停止并撤下控件：运行 Stop-Codex-Dashboard.ps1。'
