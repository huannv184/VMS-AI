# Phase 1 of AI-Worker AV crash debug.
# Configures Windows Error Reporting LocalDumps for ai_worker_v2.exe so that
# the next access-violation crash is captured as a full minidump WITHOUT
# rebuilding the binary. Phase 2 (in-process MiniDumpWriteDump in ai_worker
# main.cpp) provides a redundant capture path that does NOT require admin /
# registry edits.
#
# RUN AS ADMINISTRATOR (HKLM write requires elevation).
#
# Reference: https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps

param(
    [string]$DumpFolder = "D:\buidC\ai2.1\AI-Camera-System\logs\crashdumps",
    [int]$DumpCount = 10,
    # 1 = mini, 2 = full. Full needed for full thread/heap snapshot to debug
    # AVs in TensorRT/OpenCV native code.
    [int]$DumpType = 2
)

$ErrorActionPreference = 'Stop'

# Verify elevated session.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "ERROR: must run as Administrator (HKLM write)." -ForegroundColor Red
    exit 1
}

# Create dump folder.
if (-not (Test-Path $DumpFolder)) {
    New-Item -Path $DumpFolder -ItemType Directory -Force | Out-Null
    Write-Host "Created dump folder: $DumpFolder"
} else {
    Write-Host "Dump folder exists: $DumpFolder"
}

# Per-app subkey under LocalDumps.
$base = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps"
$app  = "$base\ai_worker_v2.exe"

if (-not (Test-Path $base)) {
    New-Item -Path $base -Force | Out-Null
}
if (-not (Test-Path $app)) {
    New-Item -Path $app -Force | Out-Null
}

New-ItemProperty -Path $app -Name DumpFolder -Value $DumpFolder -PropertyType ExpandString -Force | Out-Null
New-ItemProperty -Path $app -Name DumpCount  -Value $DumpCount  -PropertyType DWord        -Force | Out-Null
New-ItemProperty -Path $app -Name DumpType   -Value $DumpType   -PropertyType DWord        -Force | Out-Null

Write-Host "OK. WER LocalDumps configured for ai_worker_v2.exe:" -ForegroundColor Green
Write-Host "  DumpFolder: $DumpFolder"
Write-Host "  DumpCount : $DumpCount"
Write-Host "  DumpType  : $DumpType (1=mini, 2=full)"
Write-Host ""
Write-Host "Next AV crash of ai_worker_v2.exe will write:"
Write-Host "  $DumpFolder\ai_worker_v2.exe.<pid>.dmp"
Write-Host ""
Write-Host "To remove this config later:"
Write-Host "  Remove-Item -Path '$app' -Force"
