# Production deployment

The backend speaks plain HTTP + plain WS. **Production deployments
terminate TLS at a reverse proxy** (nginx, caddy, or IIS) and forward to
the backend on loopback. Choose one of:

- [`docker-compose.yml`](./docker-compose.yml) — full infrastructure
  stack (postgres + minio + caddy) in containers. Backend still runs
  natively on the host (Linux port pending). Best choice for a turnkey
  single-host production deploy. See the [quick-start](#quick-start-with-docker-compose)
  section below.
- [`nginx.conf.sample`](./nginx.conf.sample) — Linux/BSD standalone,
  manual cert management (Let's Encrypt via certbot, internal CA, or
  commercial).
- [`Caddyfile.sample`](./Caddyfile.sample) — Linux/Windows standalone,
  automatic Let's Encrypt provisioning + renewal. Best choice when the
  host already has its own postgres + minio.

## Topology

```
                ┌─────────────────────────┐
                │   Host (Windows/Linux)  │
   public DNS   │                         │
   vms.example  │   ┌──────────────────┐  │
   .com         │   │  vms_backend     │  │  ← native exe (NSSM/systemd)
       │        │   │  127.0.0.1:8000  │  │     server.host=127.0.0.1
       │        │   │  127.0.0.1:8083  │  │
       │        │   └────────▲─────────┘  │
       │        │            │            │
       │        │  ┌─────────┴─────────┐  │  ← compose-managed
       ▼        │  │  caddy  :443      │  │     (host.docker.internal:host-gateway)
   ┌──────┐    │  │  (TLS termination)│  │
   │ User │────┼─►│  ↓ proxy_pass      │  │
   └──────┘    │  ├───────────────────┤  │
               │  │  postgres :5432   │  │  ← compose, loopback-only
               │  │  minio :9000      │  │  ← compose, loopback-only
               │  └───────────────────┘  │
               └─────────────────────────┘
```

## Quick start with docker-compose

Prerequisites: Docker Engine 24+ (or Docker Desktop 4+), public DNS
record for `VMS_DOMAIN` pointing at this host, ports 80 + 443 open.

```bash
cd deploy/
cp .env.example .env
# Edit .env — set every value, especially the *_PASSWORD / *_KEY ones.
# Tip: openssl rand -base64 36   (for VMS_JWT_SECRET)
#      openssl rand -base64 24   (for VMS_PG_PASSWORD)
#      openssl rand -base64 18   (×2 for VMS_MINIO_*)

docker compose up -d
docker compose ps      # all three should report (healthy) within ~30s
```

Then point `cpp-backend/config/backend.yaml` at the new infra:

```yaml
server:
  host: "127.0.0.1"
  port: 8000

database:
  driver: "postgresql"
  postgresql:
    host: "127.0.0.1"
    port: 5432
    database: "vms"
    username: "vms"
    # password injected via VMS_PG_PASSWORD env (do NOT commit it here)

storage:
  driver: "minio"
  required: true        # H7: fail-fast on MinIO outage
  minio:
    endpoint: "http://127.0.0.1:9000"
    # access_key + secret_key via VMS_MINIO_* env

security:
  trusted_proxies:
    - "loopback"        # caddy proxies into us from host.docker.internal

cors:
  origins:
    - "https://vms.example.com"
  credentials: true
```

Export the secrets and start the backend:

```powershell
# Windows
$env:VMS_PG_PASSWORD       = (Get-Content deploy/.env | Select-String 'VMS_PG_PASSWORD').ToString().Split('=')[1]
$env:VMS_MINIO_ACCESS_KEY  = ...
$env:VMS_MINIO_SECRET_KEY  = ...
$env:VMS_JWT_SECRET        = ...
.\cpp-backend\build\Release\vms_backend.exe --config cpp-backend\config\backend.yaml
```

```bash
# Linux: source the .env, then run native vms_backend
set -a; source deploy/.env; set +a
./cpp-backend/build/Release/vms_backend --config cpp-backend/config/backend.yaml
```

Browse to `https://<VMS_DOMAIN>` — Caddy provisions a Let's Encrypt cert
on first hit. First page load takes a few seconds while the ACME
HTTP-01 challenge completes.

### Operational notes

- `docker compose logs -f caddy` — watch cert provisioning + proxy
  traffic.
- `docker compose down` — stop the stack (data persists in volumes).
- `docker compose down -v` — **destructive**. Drops postgres + minio
  data + Caddy's cert cache. Re-provisioning hits Let's Encrypt rate
  limits (5 certs/week/domain). Only use if you intend to start over.
- MinIO console at `https://<VMS_DOMAIN>/minio/` (root creds from
  `.env`). Remove the `handle_path /minio/*` block in
  `Caddyfile.compose` if you don't want the console reachable from the
  public domain.
- Backend is NOT managed by compose. Use NSSM (Windows) or systemd
  (Linux) to keep it alive across reboots. See
  `scripts/install_service.ps1` for the Windows path.

## Backend-side prerequisites

Match the proxy with these `cpp-backend/config/backend.yaml` entries:

```yaml
server:
  host: "127.0.0.1"          # bind loopback only — proxy is the only entry
  port: 8000

security:
  trusted_proxies:
    - "loopback"              # honour XFF / XFP from 127.0.0.1, ::1

websocket:
  port: 8083                  # backend WS port; proxy forwards /ws to it

cors:
  origins:
    - "https://vms.example.com"   # real production origin only
  credentials: true
```

When `security.trusted_proxies` is non-empty AND the proxy forwards
`X-Forwarded-Proto: https`, the backend emits HSTS automatically (see
`include/utils/security_headers.h` for the gate logic). Without those
two conditions HSTS is suppressed — a no-proxy dev box must not poison
browsers into refusing future plain-HTTP localhost connections.

## Bind-safety warnings

`main()` LOG_WARNs on two production misconfigurations at boot:

1. `server.host=0.0.0.0` + `trusted_proxies` set — backend exposed on
   the LAN as well as via the proxy; LAN clients bypass TLS.
2. `server.host=0.0.0.0` + empty `trusted_proxies` + auth enabled —
   plain HTTP everywhere; passwords + JWTs unencrypted.

Silence either with env `VMS_ALLOW_INSECURE_BIND=1` (dev / lab only).

## Cert renewal

- **Caddy**: automatic. Nothing to do once the email + domain are set.
- **certbot / nginx**: typical setup is `certbot --nginx -d
  vms.example.com` once + a systemd timer for renewal. Nginx reloads
  on SIGHUP after cert rotation.

## WebSocket gotcha

Both samples set `proxy_buffering off` (nginx) / `flush_interval -1`
(Caddy) for the `/ws` location. FMP4 video frames are binary, and
proxy-buffered binary streams get truncated mid-frame — the same trap
that bit the Vite dev proxy. See
`vms-frontend/src/utils/runtimeUrls.js:50` for the corresponding
client-side fallback (FE direct-connects to `:8083` in dev to skip the
Vite proxy entirely).
