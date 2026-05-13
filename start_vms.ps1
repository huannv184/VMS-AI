# Tự động dọn dẹp Port ma và Bật Nút Hệ thống VMS trên Windows
#
# Portability: All paths are derived from $PSScriptRoot so this script works
# on any drive / directory layout. Pre-fix had a hardcoded
#   D:\buidC\ai2.1\AI-Camera-System\cpp-backend\config\backend.yaml
# which broke deploys on machines without that exact path.

$ErrorActionPreference = 'Stop'
$RepoRoot     = $PSScriptRoot
$BackendRoot  = Join-Path $RepoRoot 'cpp-backend'
$BackendBin   = Join-Path $BackendRoot 'build\Release'
$BackendExe   = Join-Path $BackendBin 'vms_backend.exe'
$ConfigPath   = Join-Path $BackendRoot 'config\backend.yaml'
$FrontendRoot = Join-Path $RepoRoot 'vms-frontend'

Write-Host "===========================" -ForegroundColor Cyan
Write-Host "VMS Startup Script - Windows" -ForegroundColor Cyan
Write-Host "Repo: $RepoRoot" -ForegroundColor Cyan
Write-Host "===========================" -ForegroundColor Cyan

if (-not (Test-Path $BackendExe)) {
    Write-Host "ERROR: vms_backend.exe not found at $BackendExe" -ForegroundColor Red
    Write-Host "       Build the backend first (cmake --build $BackendRoot\build --config Release)." -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $ConfigPath)) {
    Write-Host "ERROR: backend.yaml not found at $ConfigPath" -ForegroundColor Red
    exit 1
}

# SEC-C1: Resolve VMS_JWT_SECRET before spawning the backend.
# The backend refuses to start (config.cpp:loadFromFile -> false) if `auth.enabled`
# is true AND the secret_key in backend.yaml is the well-known default sentinel
# "vms-ai-secret-key-change-me-in-production" AND no override env var is set.
#
# Resolution order (first hit wins):
#   1. $env:VMS_ALLOW_DEFAULT_SECRET == "1"  -> bypass guard (dev-only, logs CRITICAL).
#   2. $env:VMS_JWT_SECRET already exported and >= 32 chars -> use as-is.
#   3. $BackendRoot/.jwt_secret file exists -> read into env var.
#   4. Generate 36 random bytes via CryptoRNG, base64 -> write file + export env var.
#
# The generated file is gitignored (.gitignore root has *.jwt_secret pattern).
# Persistence across reboots is via the file, not via [Environment]::SetEnvironmentVariable
# at User scope — keeping it local to the repo avoids polluting the user profile.
$SecretFile = Join-Path $BackendRoot '.jwt_secret'
if ($env:VMS_ALLOW_DEFAULT_SECRET -eq "1") {
    Write-Host "JWT: VMS_ALLOW_DEFAULT_SECRET=1 detected -> bypassing SEC-C1 guard (dev mode)." -ForegroundColor Yellow
}
elseif ($env:VMS_JWT_SECRET -and $env:VMS_JWT_SECRET.Length -ge 32) {
    Write-Host "JWT: using VMS_JWT_SECRET from current session ($($env:VMS_JWT_SECRET.Length) chars)." -ForegroundColor Green
}
else {
    if (Test-Path $SecretFile) {
        $persisted = (Get-Content -Raw -Path $SecretFile).Trim()
        if ($persisted.Length -ge 32) {
            $env:VMS_JWT_SECRET = $persisted
            Write-Host "JWT: loaded persisted secret from $SecretFile ($($persisted.Length) chars)." -ForegroundColor Green
        }
        else {
            Write-Host "JWT: $SecretFile exists but is too short ($($persisted.Length) chars). Regenerating." -ForegroundColor Yellow
            Remove-Item -Path $SecretFile -Force
        }
    }
    if (-not $env:VMS_JWT_SECRET) {
        $bytes = New-Object byte[] 36
        [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
        $generated = [Convert]::ToBase64String($bytes)
        Set-Content -Path $SecretFile -Value $generated -Encoding ASCII -NoNewline
        $env:VMS_JWT_SECRET = $generated
        Write-Host "JWT: generated new secret and persisted to $SecretFile (gitignored)." -ForegroundColor Green
    }
}

# 1. Diệt các tiến trình đang chiếm Port 8000/8083/3000 (chuyên trị lỗi ghost port trên Windows)
Write-Host "1. Quét và dọn dẹp Port 8000 / 8083 / 3000..." -ForegroundColor Yellow
$ports = @(8000, 8083, 3000)
foreach ($port in $ports) {
    $pids = (netstat -ano | Select-String ":$port " | ForEach-Object { ($_ -split '\s+')[-1] } | Select-Object -Unique)
    foreach ($p in $pids) {
        if ($p -ne "0" -and $p -ne "") {
            Write-Host " - Đang đóng tiến trình PID $p chiếm Port $port..." -ForegroundColor DarkYellow
            taskkill /F /PID $p /T *>$null
        }
    }
}
Write-Host " Hoàn tất dọn dẹp." -ForegroundColor Green

# 2. Chạy C++ Backend (ẩn)
Write-Host "2. Đang khởi động Backend (Background)..." -ForegroundColor Yellow
Start-Process -FilePath $BackendExe `
              -WorkingDirectory $BackendBin `
              -ArgumentList "--config","`"$ConfigPath`"" `
              -WindowStyle Hidden
Write-Host " Backend đã chạy! (Logs: $BackendBin\logs\)" -ForegroundColor Green

# 3. Chạy lệnh Frontend
Write-Host "3. Đang khởi động Frontend (Vite)..." -ForegroundColor Yellow
Push-Location $FrontendRoot
try {
    npm run dev
} finally {
    Pop-Location
}
