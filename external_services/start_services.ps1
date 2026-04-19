# Start VMS Infrastructure (Portable Windows)
$baseDir = Get-Location
$infraDir = Join-Path $baseDir "external_services"
$pgBin = Join-Path $infraDir "postgresql\bin"
$pgData = Join-Path $infraDir "data\postgres"
$minioExe = Join-Path $infraDir "minio.exe"
$minioData = Join-Path $infraDir "data\minio"
$mtxExe = Join-Path $infraDir "mediamtx\mediamtx.exe"

Write-Host "--- VMS Service Manager ---" -ForegroundColor Cyan

# Check if PostgreSQL is actually listening on port 5432
$pgListening = Get-NetTCPConnection -LocalPort 5432 -State Listen -ErrorAction SilentlyContinue
if ($pgListening) {
    Write-Host "PostgreSQL is already listening on port 5432." -ForegroundColor Gray
} else {
    Write-Host "PostgreSQL is not responding on port 5432. Attempting to start..." -ForegroundColor Green
    
    # Try to clean up stale PID file if it exists
    if (Test-Path "$pgData\postmaster.pid") {
        Write-Host "Cleaning up stale postmaster.pid..." -ForegroundColor Yellow
        Remove-Item "$pgData\postmaster.pid" -Force -ErrorAction SilentlyContinue
    }

    $pgCtl = Join-Path $pgBin "pg_ctl.exe"
    Start-Process -FilePath $pgCtl -ArgumentList "start -D `"$pgData`"" -NoNewWindow
    
    # Wait a bit for startup
    Start-Sleep -Seconds 2
}

# Check if MinIO is already running
$minioProc = Get-Process | Where-Object {$_.Name -eq "minio"}
if ($minioProc) {
    Write-Host "MinIO is already running." -ForegroundColor Gray
} else {
    Write-Host "Starting MinIO..." -ForegroundColor Green
    $env:MINIO_ROOT_USER = "minioadmin"
    $env:MINIO_ROOT_PASSWORD = "minioadmin"
    Start-Process -FilePath $minioExe -ArgumentList "server `"$minioData`" --console-address :9001" -NoNewWindow
}

# Check if MediaMTX is already running
$mtxProc = Get-Process | Where-Object {$_.Name -eq "mediamtx"}
if ($mtxProc) {
    Write-Host "MediaMTX is already running." -ForegroundColor Gray
} else {
    if (Test-Path $mtxExe) {
        Write-Host "Starting MediaMTX..." -ForegroundColor Green
        Start-Process -FilePath $mtxExe -WorkingDirectory (Split-Path $mtxExe) -NoNewWindow
    }
}

Write-Host "---------------------------" -ForegroundColor Cyan
Write-Host "MinIO API: http://localhost:9000"
Write-Host "MinIO Console: http://localhost:9001 (minioadmin / minioadmin)"
Write-Host "PostgreSQL Port: 5432 (User: postgres, DB: vms_ai)"
Write-Host "MediaMTX RTSP: rtsp://localhost:8554"
Write-Host "---------------------------"
