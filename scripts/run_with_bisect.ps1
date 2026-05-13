# Run vms_backend with AI-worker model bisect env vars set. The env vars are
# inherited by every child ai_worker_v2.exe spawned by camera_pipeline_manager,
# so flipping them here flips the model for all cameras at once.
#
# Usage:
#   .\scripts\run_with_bisect.ps1                       # baseline (all defaults)
#   .\scripts\run_with_bisect.ps1 -DisableFace          # face/recog OFF
#   .\scripts\run_with_bisect.ps1 -DisableFace -DisableLpr -DisableFire
#
# Bisect plan to isolate the AV in cam 3:
#   1) baseline — confirm crash reproduces
#   2) -DisableFace — if no crash, face pipeline (SCRFD/ArcFace/FaceTracker)
#                     is the suspect; minidump from baseline tells us which
#   3) -DisableLpr  — usually default OFF, only flip if face-disable still crashes
#   4) -DisableYolo — last resort; without YOLO no objects = no detections at
#                     all but worker stays alive

param(
    [switch]$DisableYolo,
    [switch]$DisableFace,
    [switch]$DisableLpr,
    [switch]$DisableFire
)

$ErrorActionPreference = 'Stop'

if ($DisableYolo) { $env:VMS_AI_DISABLE_YOLO = '1' } else { Remove-Item Env:VMS_AI_DISABLE_YOLO -ErrorAction Ignore }
if ($DisableFace) { $env:VMS_AI_DISABLE_FACE = '1' } else { Remove-Item Env:VMS_AI_DISABLE_FACE -ErrorAction Ignore }
if ($DisableLpr ) { $env:VMS_AI_DISABLE_LPR  = '1' } else { Remove-Item Env:VMS_AI_DISABLE_LPR  -ErrorAction Ignore }
if ($DisableFire) { $env:VMS_AI_DISABLE_FIRE = '1' } else { Remove-Item Env:VMS_AI_DISABLE_FIRE -ErrorAction Ignore }

Write-Host "AI-worker bisect env:" -ForegroundColor Cyan
Write-Host "  VMS_AI_DISABLE_YOLO = $($env:VMS_AI_DISABLE_YOLO)"
Write-Host "  VMS_AI_DISABLE_FACE = $($env:VMS_AI_DISABLE_FACE)"
Write-Host "  VMS_AI_DISABLE_LPR  = $($env:VMS_AI_DISABLE_LPR)"
Write-Host "  VMS_AI_DISABLE_FIRE = $($env:VMS_AI_DISABLE_FIRE)"
Write-Host ""

# Reuse existing port-cleanup + frontend launch from start_vms.ps1
$ports = @(8000, 8083, 3000)
foreach ($port in $ports) {
    $pids = (netstat -ano | Select-String ":$port " | ForEach-Object { ($_ -split '\s+')[-1] } | Select-Object -Unique)
    foreach ($p in $pids) {
        if ($p -ne "0" -and $p -ne "") {
            taskkill /F /PID $p /T *>$null
        }
    }
}

Push-Location "cpp-backend\build\Release"
try {
    Write-Host "Launching vms_backend.exe foreground (Ctrl+C to stop)..." -ForegroundColor Yellow
    Write-Host "Logs: cpp-backend\build\Release\logs\cpp_backend.log"
    Write-Host "Crash dumps: logs\crashdumps\ai_worker_cam<id>_pid<pid>_<ts>.dmp"
    Write-Host ""
    & ".\vms_backend.exe" --config "D:\buidC\ai2.1\AI-Camera-System\cpp-backend\config\backend.yaml"
} finally {
    Pop-Location
}
