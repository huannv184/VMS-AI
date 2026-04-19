# Tự động dọn dẹp Port ma và Bật Nút Hệ thống VMS trên Windows

Write-Host "===========================" -ForegroundColor Cyan
Write-Host "VMS Startup Script - Windows" -ForegroundColor Cyan
Write-Host "===========================" -ForegroundColor Cyan

# 1. Diệt các tiến trình đang chiếm Port 8000 và 8083 (chuyên trị lỗi ghost port trên Windows)
Write-Host "1. Quét và dọn dẹp Port 8000 / 8083..." -ForegroundColor Yellow
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
Push-Location "cpp-backend\build\Release"
Start-Process -FilePath ".\vms_backend.exe" -WorkingDirectory (Get-Location).Path -ArgumentList "--config D:\buidC\ai2.1\AI-Camera-System\cpp-backend\config\backend.yaml" -WindowStyle Hidden
Pop-Location
Write-Host " Backend đã chạy! (Các log được ghi vào cpp-backend\build\Release\logs\)" -ForegroundColor Green

# 3. Chạy lệnh Frontend
Write-Host "3. Đang khởi động Frontend (Vite)..." -ForegroundColor Yellow
Push-Location "vms-frontend"
npm run dev
Pop-Location
