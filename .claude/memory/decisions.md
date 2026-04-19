# Architectural Decisions — AI Camera System

## 2026-04-18 Security Remediation Review

### Decision: Strip credentials from /api/devices/:id/channels response
- **Choice**: Return RTSP path segments only (`rtsp_path`, `rtsp_sub_path`) instead of full `rtsp://user:pass@host/...` URLs
- **Rationale**: Credentials must never leave the server. Clients that need to stream use the backend proxy (e.g. `/api/cameras/:id/stream`). The RTSP path is sufficient for the frontend to understand channel topology.
- **Trade-off**: Frontend that used the full RTSP URL directly (e.g. for VLC playback) must be updated to use the proxy endpoint instead.

### Decision: Use `weakly_canonical` + prefix guard for recording file deletion
- **Choice**: `std::filesystem::weakly_canonical` followed by `rfind(rec_root, 0) == 0` check
- **Rationale**: `canonical()` throws if file doesn't exist yet; `weakly_canonical` resolves symlinks for existing components without requiring the leaf to exist, which matches the delete-if-exists pattern. Prefix guard prevents traversal even through symlinks.

---

## 2026-04-18 Phase-2 Architecture Decisions

### Decision: token_version for JWT revocation (not a revocation list)
- **Choice**: `token_version INTEGER` column in users table; JWT embeds `ver` claim; mismatch rejects token
- **Rationale**: A per-jti revocation list requires persistent storage and O(1) lookup per request, with unbounded growth. `token_version` adds one integer per user; revocation is as simple as `UPDATE users SET token_version = token_version + 1 WHERE id = ?`. One DB read per JWT auth request (cached by connection pool).
- **Trade-off**: Revokes all tokens for the user (no per-device granularity). For per-device revocation, add a `sessions` table with jti → device_id mapping.

### Decision: RBAC as role → permission set (not per-endpoint ACL table)
- **Choice**: Static `rolePermissions()` map in `api_utils.h`; `requirePermission(ctx, perm, origin)` at handler entry
- **Rationale**: A DB-backed ACL table adds a query per request and can get out of sync. The permission model is stable (admin/operator/viewer); it can be changed in code and redeployed. For customer-configurable RBAC, switch to DB-backed permissions stored in the existing `roles.permissions` JSON column.
- **Trade-off**: Role changes require redeployment. Mitigated: `role_id` comes from DB (re-validated each request via H2 fix), so demoting a user takes effect immediately.

### Decision: HttpOnly cookie as primary auth; localStorage for WS only
- **Choice**: Set `vms_session` cookie on login; `credentials: 'include'` in all fetch calls; localStorage token kept only for WS AUTH message
- **Rationale**: Browser `<img>/<video>/fetch` all send cookies automatically. WS cannot send cookies as protocol headers, so WS auth still uses the token from localStorage. This is a known gap — mitigation is a short-lived WS ticket endpoint (`POST /api/auth/ws-ticket` → returns a 30s single-use ticket) in a future sprint.
- **Trade-off**: Requires `SameSite=Strict` + `Secure` — breaks `http://` dev environments. Remove `Secure` flag for local dev using `VMS_ENV=dev`.

### Decision: Argon2id with compile-time guard (VMS_HAS_ARGON2)
- **Choice**: Argon2 implementation behind `#ifdef VMS_HAS_ARGON2`; graceful fallback to per-user SHA256
- **Rationale**: Argon2 requires `libargon2` from vcpkg. Rather than breaking the existing build, the guard lets the build succeed without argon2 while making the intent clear. SHA256 auto-upgrade still runs on login.
- **How to activate**: Add `"argon2"` to `vcpkg.json`, `find_package(argon2 CONFIG REQUIRED)` to CMakeLists.txt, add `-DVMS_HAS_ARGON2` to compiler flags.

### Decision: inflight_bytes_ atomic map instead of bytesToWrite() for backpressure
- **Choice**: `std::unordered_map<const void*, std::atomic<int>> inflight_bytes_` keyed by raw socket pointer
- **Rationale**: `QWebSocket::bytesToWrite()` is not thread-safe to call from a non-Qt thread. The atomic counter is incremented by the producer (decoder thread) before posting the invokeMethod, and decremented inside the lambda on the Qt thread after the send. No mutex needed because the counter only needs approximate accuracy for backpressure decisions.
- **Trade-off**: The counter accumulates even for queued-but-not-yet-sent messages; it may over-estimate in-flight bytes if the Qt thread is busy. This is acceptable — over-dropping is safer than under-dropping.

### Decision: `authRejected` ref in useWebSocket to stop reconnect storm
- **Choice**: Single boolean ref, cleared on re-login event, set by AUTH_FAILED message or WS close code 1008
- **Rationale**: Minimal state; avoids modifying the reconnect timeout logic which handles transient network failures correctly. Re-login fires `vms_auth_changed` which resets the flag.
