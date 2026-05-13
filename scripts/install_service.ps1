# Register VMS backend as a Windows Service (NSSM-managed) so it auto-starts
# on boot and restarts on crash. Required because:
#   - start_vms.ps1 runs vms_backend.exe foreground; on crash there is no
#     supervisor to restart it (the AI worker has BUG-PM-RESTART-01 backoff
#     internally, but vms_backend.exe itself has no parent watchdog).
#   - Operators currently re-run the script after every reboot. A service
#     removes that toil and gives Event Viewer a place to capture failures.
#
# Prereqs:
#   1. NSSM installed and on PATH:
#        choco install nssm        (Chocolatey)
#        scoop install nssm         (Scoop)
#        or download from https://nssm.cc/download and add to PATH
#   2. RUN AS ADMINISTRATOR.
#   3. Backend has been built (cpp-backend\build\Release\vms_backend.exe exists).
#   4. backend.yaml has a real VMS_JWT_SECRET configured (see SEC-C1 guard in
#      cpp-backend/src/utils/config.cpp:175). The service environment block
#      below shows where to inject it.
#
# Usage:
#   .\scripts\install_service.ps1                    # install + start
#   .\scripts\install_service.ps1 -Action start      # start only
#   .\scripts\install_service.ps1 -Action stop       # stop only
#   .\scripts\install_service.ps1 -Action remove     # uninstall
#   .\scripts\install_service.ps1 -Action status     # query

param(
    [ValidateSet('install','remove','start','stop','restart','status')]
    [string]$Action = 'install',
    [string]$ServiceName = 'VmsBackend',
    [string]$DisplayName = 'VMS AI Camera Backend',
    [string]$JwtSecret = $env:VMS_JWT_SECRET
)

$ErrorActionPreference = 'Stop'

$RepoRoot   = Split-Path -Parent $PSScriptRoot
$BackendDir = Join-Path $RepoRoot 'cpp-backend\build\Release'
$BackendExe = Join-Path $BackendDir 'vms_backend.exe'
$ConfigPath = Join-Path $RepoRoot 'cpp-backend\config\backend.yaml'
$LogDir     = Join-Path $BackendDir 'logs\service'

# --- Helpers -----------------------------------------------------------------

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p  = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "ERROR: must run as Administrator (service registration)." -ForegroundColor Red
        exit 1
    }
}

function Find-Nssm {
    $cmd = Get-Command nssm -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in @(
        'C:\ProgramData\chocolatey\bin\nssm.exe',
        "$env:USERPROFILE\scoop\apps\nssm\current\nssm.exe",
        "$env:LOCALAPPDATA\Programs\nssm\nssm.exe"
    )) {
        if (Test-Path $p) { return $p }
    }
    Write-Host "ERROR: nssm.exe not found on PATH." -ForegroundColor Red
    Write-Host "       Install via: choco install nssm   (or)   scoop install nssm" -ForegroundColor Red
    Write-Host "       Or download from https://nssm.cc/download" -ForegroundColor Red
    exit 1
}

function Get-ServiceStatus {
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($null -eq $svc) { return 'NOT_INSTALLED' }
    return $svc.Status.ToString().ToUpper()
}

# --- Actions -----------------------------------------------------------------

function Do-Install {
    Assert-Admin
    $nssm = Find-Nssm

    if (-not (Test-Path $BackendExe)) {
        Write-Host "ERROR: backend not built at $BackendExe" -ForegroundColor Red
        Write-Host "       Build first:" -ForegroundColor Red
        Write-Host "         cmake --build $RepoRoot\cpp-backend\build --config Release" -ForegroundColor Red
        exit 1
    }
    if (-not (Test-Path $ConfigPath)) {
        Write-Host "ERROR: backend.yaml missing at $ConfigPath" -ForegroundColor Red
        exit 1
    }
    if ([string]::IsNullOrWhiteSpace($JwtSecret) -or $JwtSecret.Length -lt 32) {
        Write-Host "ERROR: VMS_JWT_SECRET must be set (>= 32 chars) before installing." -ForegroundColor Red
        Write-Host "       Either:" -ForegroundColor Red
        Write-Host "         `$env:VMS_JWT_SECRET = '<32+ random chars>'" -ForegroundColor Red
        Write-Host "       or pass -JwtSecret <secret>." -ForegroundColor Red
        Write-Host "       The backend's SEC-C1 guard will refuse to start without it (config.cpp:175)." -ForegroundColor Red
        exit 1
    }
    if (-not (Test-Path $LogDir)) {
        New-Item -Path $LogDir -ItemType Directory -Force | Out-Null
    }

    if ((Get-ServiceStatus) -ne 'NOT_INSTALLED') {
        Write-Host "Service '$ServiceName' already exists. Removing first..." -ForegroundColor Yellow
        Do-Remove
    }

    Write-Host "Installing service '$ServiceName'..." -ForegroundColor Cyan
    & $nssm install $ServiceName $BackendExe '--config' $ConfigPath | Out-Null
    & $nssm set $ServiceName DisplayName $DisplayName | Out-Null
    & $nssm set $ServiceName Description 'VMS AI Camera Backend (HTTP API + RTSP pipeline + AI worker supervisor)' | Out-Null
    & $nssm set $ServiceName AppDirectory $BackendDir | Out-Null

    # Restart policy: restart on any non-zero exit, throttle to 60s between
    # restarts so a tight crash loop doesn't pin a CPU core. NSSM defaults
    # are aggressive (~250ms); 60s matches our internal AI-worker backoff.
    & $nssm set $ServiceName AppExit Default Restart       | Out-Null
    & $nssm set $ServiceName AppRestartDelay 60000         | Out-Null
    & $nssm set $ServiceName AppThrottle 30000             | Out-Null

    # Stdout/stderr → log files in $LogDir, rotated daily/16MB.
    & $nssm set $ServiceName AppStdout  (Join-Path $LogDir 'service-stdout.log') | Out-Null
    & $nssm set $ServiceName AppStderr  (Join-Path $LogDir 'service-stderr.log') | Out-Null
    & $nssm set $ServiceName AppRotateFiles 1                  | Out-Null
    & $nssm set $ServiceName AppRotateOnline 1                 | Out-Null
    & $nssm set $ServiceName AppRotateBytes  16777216          | Out-Null

    # Environment variables — pass JWT secret + any model overrides.
    # NSSM AppEnvironmentExtra accepts CRLF-separated KEY=VALUE pairs.
    $envBlock = "VMS_JWT_SECRET=$JwtSecret`r`n"
    & $nssm set $ServiceName AppEnvironmentExtra $envBlock     | Out-Null

    # Auto-start on boot, but with delayed-start so SQL Server / MediaMTX
    # have time to come up first. (NSSM exposes Windows SC start type.)
    & $nssm set $ServiceName Start SERVICE_DELAYED_AUTO_START  | Out-Null

    Write-Host "Service installed. Starting..." -ForegroundColor Cyan
    & $nssm start $ServiceName | Out-Null
    Start-Sleep -Seconds 3
    Write-Host "Status: $(Get-ServiceStatus)" -ForegroundColor Green
    Write-Host ""
    Write-Host "Logs:    $LogDir\service-stdout.log" -ForegroundColor Gray
    Write-Host "Stop:    .\scripts\install_service.ps1 -Action stop" -ForegroundColor Gray
    Write-Host "Remove:  .\scripts\install_service.ps1 -Action remove" -ForegroundColor Gray
}

function Do-Remove {
    Assert-Admin
    $nssm = Find-Nssm
    $st = Get-ServiceStatus
    if ($st -eq 'NOT_INSTALLED') { Write-Host "Not installed."; return }
    if ($st -eq 'RUNNING' -or $st -eq 'STARTPENDING') {
        Write-Host "Stopping..." -ForegroundColor Yellow
        & $nssm stop $ServiceName | Out-Null
        Start-Sleep -Seconds 2
    }
    Write-Host "Removing service '$ServiceName'..." -ForegroundColor Cyan
    & $nssm remove $ServiceName confirm | Out-Null
    Write-Host "Removed." -ForegroundColor Green
}

function Do-Start {
    Assert-Admin
    Find-Nssm | Out-Null
    Start-Service $ServiceName
    Write-Host "Status: $(Get-ServiceStatus)" -ForegroundColor Green
}

function Do-Stop {
    Assert-Admin
    Find-Nssm | Out-Null
    Stop-Service $ServiceName
    Write-Host "Status: $(Get-ServiceStatus)" -ForegroundColor Green
}

function Do-Restart {
    Do-Stop; Start-Sleep -Seconds 2; Do-Start
}

function Do-Status {
    $st = Get-ServiceStatus
    Write-Host "Service '$ServiceName': $st" -ForegroundColor Cyan
    if ($st -ne 'NOT_INSTALLED') {
        Get-Service $ServiceName | Format-List Name, Status, StartType, DisplayName
        Get-CimInstance Win32_Service -Filter "Name='$ServiceName'" |
            Select-Object Name, ProcessId, StartMode, State, PathName |
            Format-List
    }
}

# --- Main --------------------------------------------------------------------

switch ($Action) {
    'install' { Do-Install }
    'remove'  { Do-Remove  }
    'start'   { Do-Start   }
    'stop'    { Do-Stop    }
    'restart' { Do-Restart }
    'status'  { Do-Status  }
}
