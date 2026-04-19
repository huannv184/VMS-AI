# AI Camera System

## Windows Build & Run

### Environment

Set these environment variables before building or running:

```powershell
$env:PORT="8000"
$env:WS_PORT="8083"
$env:VITE_API_URL="http://127.0.0.1:8000"
$env:VITE_WS_URL="ws://127.0.0.1:8083/ws"
$env:VMS_ENV="dev"
```

`config/.env.example` contains the same values for reference.

### Backend

Build Release x64:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "cpp-backend\build\vms_backend.vcxproj" `
  /p:Configuration=Release /p:Platform=x64 /t:Build
```

Run:

```powershell
cd cpp-backend\build\Release
.\vms_backend.exe --config ..\..\config\backend.yaml
```

The backend serves `vms-frontend/dist` directly when the frontend has been built.

### Frontend

Install and build:

```powershell
cd vms-frontend
npm install
npm run build
```

Serve options:

1. Preferred: let the backend serve `vms-frontend/dist`.
2. Fallback: use `npm run serve`.
3. Alternate reverse proxy: use `vms-frontend/nginx.conf` with Windows nginx.

### Ports

- Backend HTTP: `PORT` default `8000`
- WebSocket: `WS_PORT` default `8083`
- Frontend build/runtime: `VITE_API_URL`, `VITE_WS_URL`
