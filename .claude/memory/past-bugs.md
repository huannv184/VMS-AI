# Past Bugs — AI Camera System

## 2026-04-18 Security Review

### [FIXED] C5 — Admin credentials prefilled in LoginView
- **File**: `vms-frontend/src/views/LoginView.jsx:6-7`
- **Bug**: `useState('admin')` for username and password — one-click admin login in production
- **Fix**: Changed to `useState('')`

### [FIXED] C2 — GET /api/devices returned plaintext device passwords
- **File**: `cpp-backend/src/api/device_controller.cpp:88`
- **Bug**: `SELECT *` included `password` column in all devices listing response
- **Fix**: Explicit column allowlist excluding `password`

### [FIXED] C3 — GET /api/devices/:id/channels leaked RTSP URLs with embedded credentials
- **File**: `cpp-backend/src/api/device_controller.cpp` (channels handler)
- **Bug**: Built `rtsp://user:pass@host:port/...` and returned to any authenticated caller
- **Fix**: Return only RTSP path segments; credentials stay server-side

### [FIXED] H1 — Null optional dereference in change-password and reset-password
- **File**: `cpp-backend/src/api/user_controller.cpp:315` and `:357`
- **Bug**: `user->salt` / `u->username` accessed without `has_value()` check on `std::optional<User>`. UB if user deleted between token issue and this call.
- **Fix**: Added `if (!user.has_value()) return 404` guards

### [FIXED] H10 — No password length validation in change/reset-password
- **File**: `cpp-backend/src/api/user_controller.cpp`
- **Bug**: Accepted empty strings and 1-char passwords
- **Fix**: Reject if `new_pass.size() < 8 || > 256`

### [FIXED] H11 — Path traversal via DB video_path in recording DELETE
- **File**: `cpp-backend/src/api/recording_controller.cpp:222-232`
- **Bug**: `std::filesystem::remove(event_opt->video_path)` with raw DB value — compromised DB could delete arbitrary files
- **Fix**: `weakly_canonical()` + prefix check against `recordings/` root before any delete

### [FIXED] H13 — useWebSocket reconnects indefinitely on AUTH_FAILED
- **File**: `vms-frontend/src/hooks/useWebSocket.js`
- **Bug**: On WS close, always reconnected if token present — hammered server if token was revoked/invalid
- **Fix**: Track `authRejected` ref; stop reconnecting on close code 1008 or `AUTH_FAILED` message; clear on re-login

### [FIXED] M3 — DELETE /api/devices used SQL string concatenation
- **File**: `cpp-backend/src/api/device_controller.cpp:251`
- **Bug**: `"DELETE FROM devices WHERE id = " + std::to_string(device_id)` — safe now (int), but pattern is fragile
- **Fix**: Prepared statement `DELETE FROM devices WHERE id = ?`

---

## 2026-04-18 Phase-2 Security Hardening (all fixed)

### [FIXED] C1 — Default JWT secret bypassed in dev mode
- **File**: `cpp-backend/src/utils/config.cpp:173`
- **Bug**: `is_dev = true` when `VMS_ENV` unset → default secret accepted silently
- **Fix**: Removed `is_dev` exception. Always block if secret == default AND `VMS_ALLOW_DEFAULT_SECRET != 1`. Added min-length (32 chars) check.

### [FIXED] C4 — RBAC missing on PTZ, ROI, alert controllers
- **Files**: `ptz_controller.cpp`, `roi_controller.cpp`, `alert_controller.cpp`
- **Bug**: All route lambdas used `[]` capture — no access to `ctx.user`
- **Fix**: Changed to `[&app]` captures + `requirePermission()` calls. Added `Permission` enum + role/permission table in `api_utils.h`.

### [FIXED] C6 — Presigned URL TTL had no ceiling
- **File**: `cpp-backend/src/utils/media_signer.cpp`
- **Bug**: `std::max(1, ttl_seconds)` — no upper bound; accepted any future `exp`
- **Fix**: `getMaxPresignTtl()` caps at configurable `max_ttl_seconds` (default 900s). Enforced in both `presignPath()` and `verifyPresign()`. Legacy unscoped fallback now gated by `allow_legacy_unsigned=false`.

### [FIXED] H2 — JWT path trusted claims without DB revalidation
- **File**: `cpp-backend/src/middleware/auth_middleware.cpp`
- **Bug**: `decodeAccessTokenJwtUser` decoded role/is_active from JWT claims only; revoked users kept access until expiry (up to 7 days)
- **Fix**: JWT path now does DB lookup + checks `is_active` + validates `token_version` claim matches DB value. `token_version` column added to users table.

### [FIXED] H4 — No login rate limiting
- **File**: `cpp-backend/src/api/user_controller.cpp`
- **Fix**: Added `RateLimiter` singleton (sliding window): 5 failures/60s per IP → 5-min lockout; 10 failures/300s per username → 15-min lockout. 24h auto-eviction.

### [FIXED] H5 — JWT in localStorage (XSS theft)
- **Files**: `cpp-backend/src/api/user_controller.cpp`, `cpp-backend/src/middleware/auth_middleware.cpp`, `vms-frontend/src/api/apiClient.js`
- **Fix**: Backend sets `Set-Cookie: vms_session=<JWT>; HttpOnly; SameSite=Strict; Secure`. Middleware reads cookie as fallback. Frontend uses `credentials: 'include'`, mirrors token in localStorage only for WS handshake.

### [FIXED] H6 — SHA256 password hashing
- **File**: `cpp-backend/src/api/user_controller.cpp`
- **Fix**: Added `password_hash.h` with argon2id (t=3, m=64MB, p=4). New passwords use argon2id when `VMS_HAS_ARGON2` defined. Legacy SHA256 auto-migrates on login.

### [FIXED] H8 — Qt cross-thread UAF in CameraStreamManager
- **Files**: `cpp-backend/src/streaming/camera_stream_manager.cpp`, `cpp-backend/include/streaming/camera_stream_manager_qt.h`
- **Bug 1**: `stop()` called `server_->close()` + `delete server_` directly from non-Qt thread (signal handler / Crow thread)
- **Fix 1**: `stop()` marshals to Qt thread via `BlockingQueuedConnection`; uses `deleteLater()` instead of direct delete
- **Bug 2**: `sendBinaryDropIfBusy` called `client->bytesToWrite()` from decoder thread (TOCTOU UAF)
- **Fix 2**: Replaced with `inflight_bytes_` atomic counter map; no direct QWebSocket calls from non-Qt threads

## Open (Not Yet Fixed)

### [OPEN] C1 — Default JWT secret committed to repo
- **File**: `cpp-backend/config/backend.yaml`, `cpp-backend/src/utils/config.cpp`
- **Risk**: Forged JWTs possible if operator ships without overriding secret

### [OPEN] C4 — RBAC missing on site/device/recording/videowall/alert/roi/reid/face/ptz mutations
- **Files**: Multiple controllers — any authenticated user can perform admin actions

### [OPEN] C6 — Presigned media URLs have no TTL ceiling
- **File**: `cpp-backend/src/middleware/auth_middleware.cpp:126-166`

### [OPEN] H2 — JWT trusts claims (role, is_active) without DB re-check on each request
- **File**: `cpp-backend/src/utils/jwt_utils.cpp`

### [OPEN] H4 — No rate limiting on /api/auth/login (brute-force)
- **File**: `cpp-backend/src/api/user_controller.cpp`

### [OPEN] H5 — JWT stored in localStorage (XSS exposure)
- **File**: `vms-frontend/src/api/apiClient.js`

### [OPEN] H6 — Weak password hashing (SHA256 vs argon2id/bcrypt)
- **File**: `cpp-backend/src/api/user_controller.cpp` `hashPasswordV2`

### [OPEN] H8 — Qt cross-thread UAF in CameraStreamManager::stop()
- **File**: `cpp-backend/src/streaming/camera_stream_manager.cpp`
