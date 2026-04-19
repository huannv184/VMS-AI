@echo off
title VMS AI Camera - Backend Server
echo ===================================================
echo     VMS AI Camera System - Backend Launcher
echo ===================================================

cd /d "%~dp0\cpp-backend\build\Release"

echo [1/2] Setting up Dynamic Link Library (DLL) Paths...
set "PATH=%~dp0cpp-backend\vcpkg_installed\x64-windows\bin;%~dp0cpp-backend\vcpkg_installed\x64-windows\tools\qt6\bin;%PATH%"

echo [2/2] Starting vms_backend.exe...
vms_backend.exe

echo.
echo VMS Backend has stopped...
pause
