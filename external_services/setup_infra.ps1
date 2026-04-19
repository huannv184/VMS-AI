# VMS Infra Setup Script (Portable Windows)
$baseDir = Get-Location
$infraDir = Join-Path $baseDir "external_services"
$pgZip = Join-Path $infraDir "postgresql.zip"
$pgDir = Join-Path $infraDir "postgresql"
$pgData = Join-Path $infraDir "data/postgres"
$minioExe = Join-Path $infraDir "minio.exe"
$minioData = Join-Path $infraDir "data/minio"
$mtxDir = Join-Path $infraDir "mediamtx"

Write-Host "--- Start VMS Infrastructure Setup ---" -ForegroundColor Cyan

# 1. Extract PostgreSQL
if (Test-Path $pgZip) {
    Write-Host "Extracting PostgreSQL..." -ForegroundColor Yellow
    if (-not (Test-Path $pgDir)) {
        Expand-Archive -Path $pgZip -DestinationPath $infraDir -Force
        # EDB zip usually has a 'pgsql' folder inside
        if (Test-Path (Join-Path $infraDir "pgsql")) {
            Rename-Item -Path (Join-Path $infraDir "pgsql") -NewName "postgresql"
        }
    }
}

# 2. Initialize PostgreSQL
$pgBin = Join-Path $pgDir "bin"
$initDb = Join-Path $pgBin "initdb.exe"
if (-not (Test-Path (Join-Path $pgData "PG_VERSION"))) {
    Write-Host "Initializing PostgreSQL database cluster..." -ForegroundColor Yellow
    & $initDb -D $pgData -U postgres --auth-local=trust --auth-host=trust
}

# 3. Download MediaMTX (RTSP/WebRTC Server)
if (-not (Test-Path $mtxDir)) {
    Write-Host "Downloading MediaMTX..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path $mtxDir -Force
    $mtxZip = Join-Path $mtxDir "mediamtx.zip"
    # Download v1.9.0 Windows amd64 release
    Invoke-WebRequest -Uri "https://github.com/bluenviron/mediamtx/releases/latest/download/mediamtx_v1.9.0_windows_amd64.zip" -OutFile $mtxZip
    Write-Host "Extracting MediaMTX..." -ForegroundColor Yellow
    Expand-Archive -Path $mtxZip -DestinationPath $mtxDir -Force
    Remove-Item $mtxZip
}

# 4. Create Start Script
$startScript = Join-Path $infraDir "start_services.ps1"
$startContent = @"
`$baseDir = Get-Location
`$pgBin = Join-Path `$baseDir "postgresql\bin"
`$pgData = Join-Path `$baseDir "data\postgres"
`$minioExe = Join-Path `$baseDir "minio.exe"
`$minioData = Join-Path `$baseDir "data\minio"
`$mtxExe = Join-Path `$baseDir "mediamtx\mediamtx.exe"

Write-Host "Starting PostgreSQL..." -ForegroundColor Green
Start-Process -FilePath (Join-Path `$pgBin "pg_ctl.exe") -ArgumentList "start -D `"`$pgData`"" -NoNewWindow

Write-Host "Starting MinIO..." -ForegroundColor Green
# Default credentials: minioadmin / minioadmin
`$env:MINIO_ROOT_USER = "minioadmin"
`$env:MINIO_ROOT_PASSWORD = "minioadmin"
Start-Process -FilePath `$minioExe -ArgumentList "server `"`$minioData`" --console-address :9001" -NoNewWindow

Write-Host "Starting MediaMTX..." -ForegroundColor Green
if (Test-Path `$mtxExe) {
    Start-Process -FilePath `$mtxExe -WorkingDirectory (Split-Path `$mtxExe) -NoNewWindow
}

Write-Host "Infrastructure started successfully!" -ForegroundColor Cyan
Write-Host "MinIO Console: http://localhost:9001"
Write-Host "PostgreSQL Port: 5432 (User: postgres)"
Write-Host "MediaMTX RTSP: rtsp://localhost:8554"
"@
Set-Content -Path $startScript -Value $startContent

Write-Host "--- Setup Complete! ---" -ForegroundColor Cyan
Write-Host "Run 'external_services\start_services.ps1' to start the system."
