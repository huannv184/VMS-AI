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

## Production deployment

The backend speaks plain HTTP + plain WS by design — TLS is terminated
at a reverse proxy (nginx, caddy, IIS). See [`deploy/`](./deploy/) for
copy-paste-ready samples and the matching `backend.yaml` snippet:

- [`deploy/nginx.conf.sample`](./deploy/nginx.conf.sample) — Linux / BSD,
  manual cert management (Let's Encrypt via certbot, internal CA, or
  commercial).
- [`deploy/Caddyfile.sample`](./deploy/Caddyfile.sample) — Linux /
  Windows, automatic Let's Encrypt provisioning + renewal.
- [`deploy/README.md`](./deploy/README.md) — full production checklist
  including `security.trusted_proxies` setup, the WS-buffering gotcha,
  and the bind-safety warnings emitted at boot when production mode is
  misconfigured.

For dev (single host, no proxy): defaults work as-is — backend on
`0.0.0.0:8000`, FE via Vite proxy on `:3000`. The bind-safety warnings
can be silenced with `VMS_ALLOW_INSECURE_BIND=1` if they're noise in a
local environment.
