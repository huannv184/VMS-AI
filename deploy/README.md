# Production deployment

The backend speaks plain HTTP + plain WS. **Production deployments
terminate TLS at a reverse proxy** (nginx, caddy, or IIS) and forward to
the backend on loopback. Two sample configs:

- [`nginx.conf.sample`](./nginx.conf.sample) — Linux/BSD, manual cert
  management (Let's Encrypt via certbot, internal CA, or commercial).
- [`Caddyfile.sample`](./Caddyfile.sample) — Linux/Windows, automatic
  Let's Encrypt provisioning + renewal. Best choice when the host has a
  public DNS name.

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
