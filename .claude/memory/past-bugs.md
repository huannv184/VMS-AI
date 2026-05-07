# Past Bugs — AI Camera System

## 2026-05-07 BUG-EVENTS-01 — 4/6 brand `pullEvents` were fake keepalive loops, hardware alarms silently dropped

- **Files**: `cpp-backend/src/core/brands/onvif_core.cpp:207`, `axis_core.cpp:132`, `dahua_core.cpp:179`, `hanwha_core.cpp:111` (pre-fix)
- **Bug**: Each of these `pullEvents` overrides:
  1. Connected to the camera (or just probed system time on ONVIF).
  2. Called the brand's `Adapter::startEventSubscription(callback, nullptr)` — which for `AxisAdapter`, `DahuaAdapter`, `HanwhaAdapter` was a `(void)callback;` no-op (verified in their adapter headers). ONVIF didn't even attempt subscription — just sent `GetSystemDateAndTime`.
  3. Spun a `while (isRunning) { sleep(500ms); onEvent("keepalive"); }` loop forever.
  Net effect: hardware events (motion, line crossing, intrusion, tamper) from any non-Hikvision/Uniview brand never reached the VMS. The only callback fired was `"keepalive"` — `camera_event_service_qt.cpp:151` filters those out (`chunk.size() > 50 && chunk.find("EventNotificationAlert")`), so they correctly went nowhere; but no real chunks were ever delivered either. Logs showed `"polling events for cam X"` and the adapter probe success → operators believed events were wired.
- **Status**: CRITICAL operational lie; production-callable for every ONVIF/Axis/Dahua/Hanwha camera in the deployment. Detection requires actively triggering an alarm at the hardware and noticing nothing reaches the dashboard — easy to miss during smoke-tests that focus on the working Hikvision path.
- **Detection**: Audit pass on 2026-05-07. Confirmed by reading each brand adapter's `startEventSubscription` override and seeing they're `(void)callback;` stubs (`AxisAdapter.hpp:41`, `DahuaAdapter.hpp:44`, `HanwhaAdapter.hpp:37`). ONVIF's `pullEvents` body itself was the smoking gun — `GetSystemDateAndTime` is not an event subscription endpoint, and the keepalive while-loop made it obvious that no real events would ever flow.
- **Fix**: Each `pullEvents` now (a) emits a single `LOG_WARN("[Brand] Hardware event subscription is NOT implemented for {host}:{port}...")` so the limitation is visible in logs, (b) sleeps 60s, (c) returns. The caller's outer worker_loop in `camera_event_service_qt.cpp:130` adds another 5s cooldown — net reconnect cadence ~65s for unsupported brands instead of the previous 500ms keepalive churn. Hikvision (uses `ISAPI/Event/notification/alertStream` long-poll via `rawStreamGet`) and Uniview (uses `LAPI/V1.0/System/Event/Subscription` via `streamGet`) keep their real implementations untouched.
- **Why warn-and-sleep instead of implementing the real subscription**: each brand's real event subscription is a multi-hundred-line HTTP/SOAP feature (ONVIF `CreatePullPointSubscription` + `PullMessages` flow with namespaced SOAP envelopes; Axis VAPIX 3 event stream; Dahua `/cgi-bin/eventManager.cgi?action=attach` multipart; Hanwha SUNAPI). Each is its own dedicated session and needs camera-hardware testing. The fail-loud warning bridges the gap until those land — silent stubs are worse than visible "not implemented".
- **Detection lesson**: When you see N parallel implementations of the same operation (one per brand), audit them as a SET. If 2 of 6 brands are real and 4 of 6 share a "fake while-loop with dead callback" shape, the divergence itself is the smoking gun. Before this audit we'd already used the same lesson on `AlertManager::sendEmail` vs `AlertRouter::sendEmail` — copy-paste-but-divergent is a strong reliability hazard.

## 2026-05-07 BUG-REC-02 — `BufferPipeline` and `ContinuousRecorder` discarded DB write bool returns

- **Files**: `cpp-backend/src/recording/buffer_pipeline.cpp:185,265`, `continuous_recorder.cpp:252` (pre-fix)
- **Bug**: Three call sites called `EventRepository::updateEventVideo` / `SegmentRepository::insertSegment` (both `bool`-returning) and discarded the return value. In `buffer_pipeline.cpp:185` the call was wrapped in `try { ... } catch (...) {}` — if the DB connection died or a constraint failed, the exception was swallowed silently and the recording on disk had no DB row. Same shape at line 265 (post-MinIO-upload DB update). In `continuous_recorder.cpp:252` the `insertSegment` failure was even more dangerous: the next retention sweep (`pruneOldSegments`) classifies un-registered segments as orphans and may DELETE valid recent recordings.
- **Status**: HIGH severity (data loss risk on continuous recorder; playback gap on event recordings). Both reachable from production hot paths.
- **Fix**: Each call now checks the bool and logs `LOG_ERROR` with the relevant identifier (`event_id` / `camera_id` / file path) on failure. Empty `catch (...)` blocks replaced with logging variants that at least record the exception. Failure does NOT abort the pipeline — the file is still on disk and the operator now has a clear log signal to investigate the DB drift.
- **Detection lesson**: `try { dbWrite(); } catch (...) {}` is the worst possible exception-handling pattern. It hides every reason the DB write might fail (connection drop, schema mismatch, constraint, lock timeout). At minimum log the exception. Better: rethrow if recovery isn't possible at this layer. We had this exact lesson in BUG-DB-01 (event.id empty → INSERT OR IGNORE silent drop) — same shape here, different table.

## 2026-05-07 BUG-INFER-01 — `FaceInfer::extractFaceFeature` discarded engine bool, silently produced zero embeddings

- **File**: `cpp-backend/src/ai/inference/modules/face_infer/face_infer.cpp:7-12` (pre-fix)
- **Bug**: `engine_->infer(faceImage, feature)` returned a bool but the caller ignored it. Then `feature.resize(512, 0.f)` filled any uninitialised tail with zeros. So a failed inference (`infer()` returns false leaving `feature` empty) produced a 512-dim all-zero vector and the function still returned `true`. Downstream cosine similarity treats a zero vector as orthogonal to every real embedding — search returns "unknown for everything" but never an error.
- **Status**: Latent. The `FaceInfer` class had ZERO instantiators in cpp-backend (production face flow goes via `MultiModelInfer::extractFaceEmbeddings`, which DOES check the bool return). But the module is built into the inference library and would have shipped the bug to anyone wiring it next.
- **Detection**: Audit pass on 2026-05-07 grep'd for `Stub|MOCK|placeholder` and traced `engine_->infer` callers; the missing `if (!ok) return false;` jumped out.
- **Fix**: Propagate the bool. Also enforce the 512-dim ArcFace contract — if the engine produced a different-sized tensor (model swapped without code update), clear and return false rather than zero-padding to 512. Constructor now `throw std::invalid_argument` if `engine` is null.
- **Detection lesson**: When a function-call's `bool` return is discarded AND the caller "fixes up" the output buffer with `resize(N, default)`, double-check what happens on `infer-returned-false`. `vector::resize(n, v)` only fills NEW elements — partial outputs survive into the result, mixed with zeros. That's worse than all-zero (looks like a real embedding).

## 2026-05-07 BUG-INFER-02 — `AdvancedInfer` face/SCRFD/ArcFace methods returned empty / "unknown" stubs

- **File**: `cpp-backend/src/ai/inference/src/advanced_infer.cpp:463-613` (pre-fix)
- **Bug**: 7 public methods (`detectFaces`, `parseSCRFDOutput`, `nmsForFaces`, `extractFaceFeature`, `recognizeFace`, `loadDatabase`, `saveDatabase`) returned empty vector / `"unknown"` / `true` (for save/load) without doing any real work — same operational-lie shape that BUG-ROI-01 had ("returning false stub looks like a real not-found answer").
- **Status**: All 7 methods have ZERO callers today (production face flow uses `MultiModelInfer::detectFaces` + `extractFaceEmbeddings`, not the AdvancedInfer face-method overloads). But `AdvancedInfer` is heavily used for OBJECT detection — `MultiModelInfer` constructs three `AdvancedInfer` instances (yolo/fire/plate). Any new code that picks up the same instance and calls e.g. `detectFaces()` would silently get an empty list, indistinguishable from "no faces in frame".
- **Detection**: Audit pass on 2026-05-07. Same grep that caught BUG-INFER-01.
- **Fix**: Convert the 5 face-pipeline methods (`detectFaces` / `parseSCRFDOutput` / `nmsForFaces` / `extractFaceFeature` / `recognizeFace`) to throw `std::logic_error` via a shared `faceMethodNotImplemented(fn)` helper. Two database persistence methods (`loadDatabase` / `saveDatabase`) return `false` instead of throwing because boot paths often probe these opportunistically and we don't want to abort startup just because no DB file is configured. Empty-result path on `detectFaces` is intentionally NOT preserved (it would re-introduce the bug); throwing is the correct fail-loud behaviour.
- **Detection lesson** (reinforces BUG-ROI-01's): TODO stubs that `return false` / `return {}` / `return "unknown"` are an operational lie — they look like normal answers. Convert to `throw std::logic_error` until a real implementation lands. Two acceptable exceptions: (1) graceful-no-op booleans like `loadDatabase("")` that boot paths probe, (2) feature-flag-disabled paths gated by a build-time `#ifdef`. Otherwise: throw.

## 2026-05-07 BUG-ALERT-02 — Legacy AlertManager: email mock + sync CURL on event loop

- **Files**: `cpp-backend/src/core/alert_manager.cpp` (`sendEmail`, `sendWebhook`)
- **Bug 1 (HIGH)**: `AlertManager::sendEmail` was a MOCK — only `LOG_INFO(">>> [MOCK EMAIL] To: ...")` and dropped the message. Same operational lie as BUG-ALERT-01 (which fixed the new RuleEngine path in `alert_router.cpp`) but on the LEGACY `alert_rules`-table path. Any operator creating an email rule via the CRUD UI thought alerts were going out — they weren't. EventManager called this on every matching event so a misconfigured "EMAIL TO ops@…" rule looked healthy in logs while delivering nothing.
- **Bug 2 (HIGH)**: `AlertManager::sendWebhook` ran `curl_easy_perform` synchronously on `EventManager::processEvent`'s broadcast loop. No `CURLOPT_TIMEOUT` was set → libcurl falls back to OS default (~120s) → one dead webhook host stalled the entire event broadcast pipeline. With the inline AlertManager dispatch under `rules_mutex_`, a single dead webhook also serialised every subsequent event evaluation.
- **Detection**: Audit pass on 2026-05-07 grep'd for `MOCK|stub|placeholder` and found the email function. Webhook hot-path issue surfaced in code review — same shape as BUG-ALERT-01 which we'd already fixed on the new path.
- **Fix**: Extracted SMTP transport into `vms::utils::sendEmailAsync` (`cpp-backend/include/utils/email_sender.h`, `src/utils/email_sender.cpp`) — single shared path now used by BOTH `AlertManager::sendEmail` (legacy) and `AlertRouter::sendEmail` (RuleEngine). Helper:
  - reads `smtp_host/smtp_from/smtp_user/smtp_pass/smtp_tls/smtp_port` from `settings` table on every send (uncached so admin updates apply without restart);
  - sanitises CR/LF + length-bounds From, To and Subject (header-injection hardening);
  - caps body at 8 KB;
  - queues every send through a dedicated `BackgroundJobRunner("email-sender", 2 workers, queue 128)` with drop-on-full + `LOG_THROTTLED_WARN`;
  - sets `CURLOPT_TIMEOUT=15s`, `CURLOPT_CONNECTTIMEOUT=10s`, `CURLOPT_NOSIGNAL=1`.

  Webhook hot-path: `AlertManager::sendWebhook` now builds the JSON payload on the calling thread (so the queued job doesn't hold a stale reference) and submits to a separate `BackgroundJobRunner("alert-mgr-webhooks", 2, 128)` with the same timeout set. EventManager broadcast loop is no longer blocked by network I/O.
- **Detection lesson**: When you fix a class of operational-lie bug (BUG-ALERT-01: log-only `sendEmail/sendSMS`) on one code path, immediately grep for the same pattern across the WHOLE codebase before closing. We had a parallel legacy path with the exact same shape. Rule of thumb: if there are two implementations of "send notification", assume they share the bug.
- **Architecture lesson**: Two parallel notification systems (legacy `alert_rules` table → AlertManager; new RuleEngine → AlertRouter) is a code smell — but BC pressure means we can't delete the legacy path overnight. Compromise: extract the *transport* (the part that's actually risky and fiddly) into a shared util so the two policy layers can keep diverging on rule semantics without the SMTP/CURL plumbing drifting.

## 2026-05-04 BUG-NIGHT-01 — Night-shift attendance rollup silently mis-attributed punches

- **Files**: `cpp-backend/src/api/attendance_controller.cpp` (`queryAttendanceForDate`)
- **Bug**: For employees on overnight shifts (e.g. 22:00→06:00):
  1. SQL window was `[day, day+24h)` and `GROUP BY person_id, employee_id`. A single shift instance produced TWO rows on the daily rollup — IN-only on day N, OUT-only on day N+1 — because the punches landed in different calendar days.
  2. `late_minutes = ci_min_of_day - shift_min - grace`. For a 06:05 next-morning punch: `365 - 1320 - 0 = -955` ⇒ clamp(0) ⇒ silently "on time". An 8-hour-late punch was reported as on time. No alert, no error, just wrong data on a payroll dashboard.
- **Detection**: A worker on a 22:00→06:00 shift never showed as late even after multiple obvious late arrivals. Root cause traced by replaying the helper math by hand against a sample punch.
- **Fix**: Bucket by `shift_date` (calendar day the shift INSTANCE began on), not punch calendar date. SQL window widened to `[day-12h, day+36h)`; `GROUP BY` removed from SQL; aggregation moves to C++ map keyed on `(person_id, employee_id, shift_date_midnight)`. Late computation switches to epoch-delta from `shift_date_midnight` so a next-day punch correctly registers as 1440+ minutes since shift start. Helpers `shiftDateMidnight()` + `lateMinutesForPunch()` added; covered by `test_attendance_shift.cpp` (16 tests including the BUG-NIGHT-01 regression case).
- **Detection lesson**: Time-arithmetic in `min_of_day` units is a trap for any periodic event that crosses midnight. Convert to absolute epoch seconds (or use a "day-of-shift-instance" key) before subtracting. Also: silent-clamp-to-zero on a "late minutes" calculation is the wrong fail-safe — when the inputs would yield a negative, the answer should be "unknown" / null, not 0.

## 2026-05-03 BUILD-001 — vms_backend.exe exits with STATUS_DLL_NOT_FOUND on fresh build
- **Files**: `cpp-backend/CMakeLists.txt`
- **Bug**: `cmake --build` produced `vms_backend.exe` cleanly but launching it from `build/Release/` exited immediately with code `0xC0000135` (STATUS_DLL_NOT_FOUND), no console output. The default vcpkg DLL deploy path only copied DLLs that some target directly imported; transitive deps (e.g. `opencv_calib3d4.dll` pulled in by `opencv_dnn4.dll`, `libwebpdemux.dll` pulled in by `opencv_imgcodecs4.dll`, `abseil_dll.dll`, `libprotobuf.dll`) were missing. Symptom: silent crash, hard to diagnose without `dumpbin /DEPENDENTS`.
- **Fix**: Added a `POST_BUILD` `add_custom_command` on `vms_backend` using CMake 3.21's `$<TARGET_RUNTIME_DLLS:vms_backend>` generator expression — resolves the FULL transitive closure of imported DLLs and copies them to `$<TARGET_FILE_DIR:vms_backend>` via `cmake -E copy_if_different`. Wrapped in `if(WIN32)` so non-Windows builds are unaffected.
- **Diagnosis recipe** for future similar issues: `dumpbin.exe /DEPENDENTS vms_backend.exe`, walk transitive imports until none missing in `Release/` or `C:\Windows\System32\`.

## 2026-05-03 Audit Pass 5 — RBAC gaps in attendance + counter modules

### [FIXED] SEC-008 (CRITICAL) — GET /api/attendance, /api/attendance/export unauthenticated
- **Files**: `cpp-backend/src/api/attendance_controller.cpp:262-286,292-320`
- **Bug**: Both routes captured `[]` (no app reference) with comment `// No auth required (matches legacy route)` and called no `requirePermission`. Anyone reachable on the API port could pull the daily attendance rollup (employee_code, full_name, dept, check_in, check_out, count) for any date and download a CSV of the same. Pure PII leak — same shape as SEC-003/SEC-004 device/site holes from 2026-05-02.
- **Fix**: Added `requireAttendanceRead` helper at the top of the anonymous namespace (gates on `ANALYTICS_READ`, dev-mode bypass when `AuthConfig.enabled=false`). Lambdas changed to `[&app]` capture. Both GET endpoints now call the helper before any DB work.

### [FIXED] SEC-009 (HIGH) — GET /api/attendance/employees, /camera-roles, /health unauthenticated
- **Files**: `cpp-backend/src/api/attendance_controller.cpp:363-396, 503-530, 563-575`
- **Bug**: GET on `/employees` returned full roster (id, person_id, code, full_name, dept, shift_id, active) to any caller — enables targeted phishing. GET on `/camera-roles` leaked the entry/exit/both/observe role for every camera — operational geometry. GET on `/health` exposed BulkWriter pending+dropped counters (lower-impact info leak but consistent with the others). All three branched on method but only the POST/PUT side had the admin gate.
- **Fix**: GET branches now go through `requireAttendanceRead` before DB work. POST gating on the same routes is unchanged (still admin-only inside the POST branch).

### [FIXED] SEC-010 (HIGH) — GET /api/counter/lines unauthenticated
- **Files**: `cpp-backend/src/api/counter_controller.cpp:105-138`
- **Bug**: The GET branch read `counting_lines` (id, camera_id, name, ax/ay/bx/by, direction labels, object_classes, enabled) without any auth check. PeopleCountTracker tripwire geometry is operationally sensitive — knowing where the lines sit lets an outsider walk around them.
- **Fix**: New `requireCounterRead` helper (mirrors `requireCounterAdmin`, gate is `ANALYTICS_READ`). Applied to the GET branch only; existing admin gates on POST/PUT/DELETE/reload preserved.

### Audit notes (false positives ruled out this pass)
- `AiEventProcessor::cooldown_cache_` IS bounded — `MAX_COOLDOWN_CACHE = 1000` with oldest-entry eviction at `setCooldown` (`ai_event_processor.cpp:396-401`).
- `AttendanceTracker::dedup_` IS bounded — opportunistic 24h-old GC at >50k entries, and the key is `packKey(person_id, camera_id)` (bounded by persons × cameras, not by detection rate).
- `counter_bucket_aggregator.cpp` SQL UPSERT does have a Postgres branch via `DbManager::isPostgres()` already — flagged but already fixed in a prior pass.
- `health_monitor.cpp` timer Connection NOT saved, but the timer is `parent=this` and lives only inside HealthMonitor; destruction of the QObject auto-disconnects. Not a leak.
- `recording_controller.cpp` Range path uses bounded chunk (4 MB cap from MAX_CHUNK in `getObjectRange`); response size_t handled internally by Crow body assignment, not truncated.

## 2026-05-02 Audit Pass 4 — RBAC + default-password bypass

### [FIXED] SEC-002 (CRITICAL) — Default-password gate did not block access
- **Files**: `cpp-backend/src/api/user_controller.cpp:192,2FA-verify`, `vms-frontend/src/views/LoginView.jsx:20`
- **Bug**: When admin/admin was detected (`default_password_active=true`), backend issued a full access token + `Set-Cookie` and only set an informational `password_change_required=true` flag. Frontend `LoginView` only checked `data.token` and called `onLoginSuccess()` regardless. Net effect: admin/admin granted full session — gate existed in name only. Same hole on the 2FA-verify path.
- **Fix**: Backend now returns ONLY a `password_change_pending` JWT (5-min TTL, `token_use="password_change_pending"`) and no cookie when the flag is set, on both login and 2FA-verify. New `POST /api/auth/change-password-on-login` endpoint accepts `{temp_token, new_password}`, rotates the password, clears the flag, bumps `token_version`, then issues the real access token + cookie. Frontend adds step 3 (mandatory password change) and `apiClient.changePasswordOnLogin`.

### [FIXED] SEC-003 (HIGH) — Devices module had zero RBAC
- **File**: `cpp-backend/src/api/device_controller.cpp` (all 8 routes)
- **Bug**: Routes captured `[]` not `[&app]`, never read AuthMiddleware context. Any authenticated user — including a viewer — could create/sync/reboot NVRs. NVR reboot during recording is operationally dangerous.
- **Fix**: All routes capture `[&app]` and call `ApiUtils::requirePermission(ctx, ..., origin)`. GET endpoints require `DEVICE_READ`, mutate require `DEVICE_WRITE`, reboot requires `SYSTEM_ADMIN` (operator with DEVICE_WRITE can configure but not power-cycle the NVR). Audit logs added for CREATE/UPDATE/DELETE/SYNC/REBOOT.

### [FIXED] SEC-004 (HIGH) — Sites module had zero RBAC
- **File**: `cpp-backend/src/api/site_controller.cpp`
- **Bug**: Same shape as devices — routes never checked permissions, only "is this request authenticated?".
- **Fix**: GET=SITE_READ, POST/PUT/DELETE=SITE_WRITE; audit log on every mutation.

### [FIXED] SEC-005 (HIGH) — Faces / Re-ID / VideoWall RBAC was "is logged in"
- **Files**: `face_controller.cpp:152,203,340,470,501`, `reid_controller.cpp:45,72,127,160,188`, `videowall_controller.cpp:24`
- **Bug**: Faces mutate, Re-ID `clearGallery` / `setConfig` / `searchByImage`, and all videowall mutate routes accepted any logged-in user, so a viewer could spoof an identity in the face gallery, wipe Re-ID state shared by all operators, or overwrite shared layouts.
- **Fix**: Per-method permission gating. GET → `*_READ`, mutate → `*_WRITE`. Videowall helper renamed `requireWriteAuth` → `requireVideoWallPerm` taking the required permission as a parameter. Read-only `face_search` / `reid_search` mapped to `*_READ` (they don't mutate state).

### [FIXED] SEC-006 (MEDIUM) — Settings PUT had no audit trail
- **File**: `cpp-backend/src/api/system_controller.cpp:222`
- **Bug**: `PUT /api/system/settings` mutated arbitrary key/value pairs without logging — no record of who changed SMTP host or LDAP bind. Also was apply-as-you-go: bad value at key 7 of 12 left a half-applied batch.
- **Fix**: Validate every value first (reject batch atomically); on apply, look up old value, write new value, then `AuditRepository::insertLog(user_id, "UPDATE_SETTING", "key 'X': old → new")`. Sensitive keys (`smtp_pass`, `twilio_auth_token`, `ldap_bind_password`, etc.) log "(value redacted)" instead.

### [FIXED] SEC-007 (MEDIUM) — Hardcoded `api_port = 8000` in runtime config
- **File**: `cpp-backend/src/api/system_controller.cpp:395`
- **Bug**: `/api/system/streaming-config` reported `api_port: 8000` regardless of actual `Config.server.port` (settable via YAML or `PORT` env var). Frontend auto-detect would build wrong URLs after deploy on 8080/8443/etc.
- **Fix**: `vms::Config::getInstance().getServerConfig().port`.

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

_None at this time._

(Note 2026-05-07: this section previously listed C1/C4/C6/H2/H4/H5/H6/H8 as `[OPEN]`,
but every one of those was fixed in the 2026-04-18 Phase-2 hardening pass
recorded in the section directly above this one. Scrubbed to avoid future
audits chasing closed issues. If you need to verify a specific fix, look for
its `[FIXED]` entry in `## 2026-04-18 Phase-2 Security Hardening`.)

## 2026-04-29

### [FIXED] BUG-HTTP-01 — Duplicate CROW_ROUTE crashes HTTP server, port 8000 never binds
- **File**: `cpp-backend/src/api/reid_controller.cpp` (pre-fix layout)
- **Bug**: Two separate `CROW_ROUTE(app, "/api/reid/config")` blocks — one with `methods(GET, OPTIONS)`, another with `methods(Put)`. Crow throws `"handler already exists for /api/reid/config"` from the HTTP-server thread during `app.run()`. Main thread never sees it; logs even print "Backend started successfully". Net effect: port 8000 silently never listens, frontend gets ECONNREFUSED on every `/api/*` call. WebSocket (port 8083→8084) still works because it's QWebSocketServer, not Crow.
- **Detection**: `Get-NetTCPConnection -OwningProcess <pid>` showed only 8084 listening, no 8000. Log line `[error] Exception in HTTP server: handler already exists for /api/reid/config` is the smoking gun.
- **Fix in source**: Single `CROW_ROUTE` with `.methods(GET, Put, OPTIONS)` and method dispatch inside the lambda. Source already updated at 2026-04-28 22:19; binary at build/Release was stale (built 22:06).
- **Followup [FIXED 2026-05-06]**: `main.cpp` now drives Crow with `HttpServer::runAsync()` (returns `std::future<void>` from `app_.run_async()`), then waits on `app_.wait_for_server_start(5s)`. If the future is ready before that wait unblocks, `run()` exited (clean or with exception) — we call `future.get()` to re-throw, log, and abort startup instead of falling through to "Backend started successfully". A second future-poll runs immediately before the success log to catch later-thread deaths (e.g. SSL handshake on first accept). Shutdown path now `future.get()`s after `http_server.stop()` instead of `server_thread.join()`.
- **Detection lesson**: Spawning `std::thread([&]{ try { x.run(); } catch(...){ LOG_ERROR; }})` and continuing the main thread independently is a common pattern that swallows startup failures. If the work item must complete before the rest of bring-up is meaningful, you need a synchronization point: either share a future, an atomic-bool + condvar, or use a synchronous start with a timeout — but never let the parent log "started" without confirming.

## 2026-05-01 Phase A latent-bug audit

### [FIXED] BUG-FB-01 — `FrameBus::publish` deep-copied raw frame on every publish with subscriber
- **File**: `cpp-backend/src/core/camera_pipeline_manager.cpp:841` (pre-fix), `cpp-backend/include/core/frame_bus.h:21`
- **Bug**: `envelope.raw_frame = frame.clone()` performed a full pixel deep-copy (~691 KB at 1080p) for every published frame. With even one subscriber and 3 cams × 15 fps that's ~31 MB/s of avoidable memcpy on the producer hot path. The struct stored a `cv::Mat` by value, so the field type itself made zero-copy fan-out impossible.
- **Fix**: `raw_frame` field changed to `std::shared_ptr<const cv::Mat>`; producer constructs `std::make_shared<const cv::Mat>(frame)` (header copy + refcount++ on pixel buffer). Verified by `FrameBus.RawFrameIsSharedZeroCopyAcrossConsumers` — pointer identity across 3 consumers.
- **Detection lesson**: When a struct that gets copied across N consumers contains a `cv::Mat` by value AND a `clone()` is happening on the producer side, you have both a deep copy AND a header copy per consumer. Use `shared_ptr<const cv::Mat>` for cross-thread fan-out.

### [FIXED] BUG-HM-01 — `HealthMonitor::start`/`stop` had three concurrent UB windows
- **File**: `cpp-backend/src/core/health_monitor.cpp:18-58` (pre-fix)
- **Bug 1**: `start()` assigns `tick_callback_` BEFORE checking `if (timer_)`. Two threads racing `start()` → both write the std::function concurrently → torn write / leaked moved-from state. (Pre-condition for the race: anything that calls `start()` more than once. Today only the manager constructor does, but adding a service that re-starts on config change would trip this.)
- **Bug 2**: `stop()` reads `timer_`, sets it to null, then dereferences the cached pointer outside any lock. Concurrent `start()` could observe `timer_ == nullptr` and `new` a second QTimer, leaving the old one to be deleted twice or used after free.
- **Bug 3**: The tick lambda reads `tick_callback_` lock-free while `stop()` clears it (or while `start()` overwrites it). std::function is not safe for concurrent read+write.
- **Fix**: Added `std::mutex lifecycle_mutex_`; `start()` is idempotent (returns if already running); `stop()` clears both members under the lock then disposes QTimer outside the lock; tick lambda copies the callback under the lock then invokes the copy outside.
- **Detection lesson**: Whenever a class has `start()` + `stop()` + an asynchronous timer/callback that touches the same fields, audit for the trio: idempotency, ordered shutdown, lock-protected callback handle. spdlog or QTimer's own thread safety doesn't cover the user state piggy-backed on them.

### [FIXED] BUG-FB-02 — `FrameBus` compacted expired slots on every publish
- **File**: `cpp-backend/src/core/frame_bus.cpp:65,87,107` (pre-fix)
- **Bug**: `compactExpiredLocked()` was called on the hot publish path, taking the bus mutex and doing a linear scan + erase even when nothing had expired. Cost: ~45 lock+scan/s with 3 cams × 15 fps × 1 subscriber on top of the fan-out itself. Not a correctness bug, but the lock window grew with subscriber count and would have shown up under the "100x current usage" load assumption.
- **Fix**: Compaction moved to the rare `subscribe()` path; expired slots are skipped implicitly via `weak_ptr::lock()` returning null in publish.
- **Detection lesson**: If a "cleanup" runs under a lock on every event, it's almost certainly in the wrong place — push it to event boundaries (subscribe/unsubscribe) and let lock() filter at read time.

### [FIXED] BUG-CTEST-01 — `ctest` exits 0xC0000135 because vcpkg modular DLLs aren't on PATH
- **File**: `cpp-backend/tests/CMakeLists.txt` (pre-fix)
- **Bug**: Modular OpenCV DLLs (`opencv_core4.dll`, etc.) live in `vcpkg_installed/x64-windows/bin` and are NOT auto-deployed next to test executables. Running `ctest` from the build directory means the loader can't find them, and every test that transitively links OpenCV crashes with STATUS_DLL_NOT_FOUND before main().
- **Fix**: `set_tests_properties(... ENVIRONMENT "PATH=<build>/$<CONFIG>\;<vcpkg>/bin\;$ENV{PATH}")` on every test target. Verified: 6/6 tests pass under `ctest -C Release`.
- **Detection lesson**: If a test runs fine when launched manually but fails under ctest, check the loader path before assuming the test itself is broken. ctest doesn't inherit an interactive shell's PATH munging.

## 2026-05-02 Module-completion pass

### [FIXED] BUG-ROI-01 — `ROIManager::isPointInROI` always returned false (silent wrong-result)
- **File**: `cpp-backend/src/core/roi_manager.cpp:104` (pre-fix)
- **Bug**: TODO stub `return false`. Any caller relying on this would silently miss every "point inside ROI" check. No active call site today, but exposed in public header — landmine for the next consumer.
- **Fix**: Parse `points_json` (canonical `[{"x":..,"y":..}, ...]` from frontend, fallback to `[[x,y],...]`), reject polygons with <3 vertices, fail-closed on pixel-coord polygons (header documents normalized 0..1; if stored polygon has any coord > 1.5 we WARN and return false rather than produce a wrong-result), use `cv::pointPolygonTest`.
- **Detection lesson**: TODO stubs that `return false` are far more dangerous than `throw` — they look like a normal "not in ROI" answer to the caller. Prefer `throw std::logic_error("not implemented")` until real impl lands.

### [FIXED] BUG-ZONE-01 — `ZoneManager::pointInPolygon` declared static, never defined
- **File**: `cpp-backend/include/events/zone_manager.h:120`, `cpp-backend/src/events/zone_manager.cpp` (pre-fix)
- **Bug**: Static helper declared in header but never given a definition. Any future call site would hit an unresolved-symbol link error. Latent — no current caller (intrusion path uses `cv::pointPolygonTest` directly, zone occupancy uses `Zone::containsPoint`).
- **Fix**: Added ray-casting definition matching `Zone::containsPoint`. Builds clean now.

### [FIXED] BUG-REC-01 — Range request loaded full MinIO object into RAM
- **File**: `cpp-backend/src/api/recording_controller.cpp:210` (pre-fix)
- **Bug**: Every authenticated `GET /api/recordings/<id>/play` Range request called `StorageManager::getObject()` which fetches the FULL object into a `std::vector<char>` before slicing. For a 200 MB clip and 10 concurrent viewers that's ~2 GB of avoidable allocation per HTTP round; misbehaved client sending many tiny ranges multiplies the cost. Header comment even acknowledged it ("full-object copy is unavoidable until storage gains a partial getObject").
- **Fix**: Added `StorageManager::getObjectRange(key, start, length)` that issues a `Range:` header and parses `Content-Range:` from the response (returns `{data, total_size, http_code}`). Recording controller now fetches only `min(client_range, 4 MB)` per request — memory is bounded regardless of file size. `MAXFILESIZE_LARGE` cap also protects against a server that ignores the Range header. Range header is intentionally NOT in SignedHeaders (matches the simplified SigV4 used for the rest of MinIO calls; works against MinIO).
- **Detection lesson**: Any HTTP byte-range path that calls a "fetch whole object" helper before slicing is a future OOM. Check the Storage abstraction expose Range natively before relying on the HTTP-controller layer to "cap output size" — the cap on output doesn't help if you already loaded full bytes to slice.

### [FIXED] BUG-PTZ-02 — `savePreset` only Hikvision; Dahua + ONVIF fell through to false
- **File**: `cpp-backend/src/core/ptz_manager.cpp:202` (pre-fix)
- **Bug**: `if (info.protocol == PTZProtocol::HIKVISION_ISAPI)` did the work and a bare `return false` swallowed everything else. Same asymmetry as deletePreset (BUG-PTZ-01) but on the other side of the lifecycle. Operators on Dahua/ONVIF saw "preset save" appear to succeed at the controller layer (controller wraps the bool in 200/500) but presets never landed on the camera.
- **Fix**: Branched switch — Dahua via `code=SetPreset`, ONVIF via new `onvifSetPreset` SOAP envelope. ISAPI body now goes through `xmlEscape` so a name like `Lobby & Door 'A'` doesn't produce malformed XML / forge sibling elements.

### [FIXED] BUG-PTZ-03 — ONVIF preset name SOAP injection surface
- **File**: `cpp-backend/src/core/ptz_manager.cpp` `onvifSetPreset` (new code)
- **Bug**: An operator with PTZ_WRITE permission could supply a `name` containing `</tptz:PresetName><tptz:SiblingThing>...` and inject SOAP elements into the request envelope. Pre-existing risk on the ISAPI body too (now fixed in BUG-PTZ-02 above).
- **Fix**: All user-supplied strings spliced into XML/SOAP go through a shared `xmlEscape` (handles `<>&"'`). Hardening, not exploited.

### [FIXED] BUG-STATS-01 — Linux CPU/RAM/disk stats hard-coded to 0
- **File**: `cpp-backend/src/utils/system_stats.cpp:177,191,209` (pre-fix)
- **Bug**: All three Linux branches returned 0. Backend deployed on Linux had blind monitoring — the metrics endpoint reported a permanently-idle box. Also: file-scope `static ULARGE_INTEGER` declarations at the top of the TU were not `#ifdef _WIN32`-guarded; the file would have failed Linux compile entirely the moment a Linux build was attempted.
- **Fix**: Linux CPU via `/proc/stat` aggregate cpu line (idle = idle+iowait, total = sum of all 8 columns; `top` convention). Linux RAM prefers `/proc/meminfo MemAvailable` (kernel-computed reclaimable estimate — sysinfo's `freeram` over-reports "used" by the page-cache amount), falls back to `sysinfo()` if /proc/meminfo unreadable. Linux disk via `statvfs("/")`. CPU model via `/proc/cpuinfo` (`model name`, falls back to `Hardware` for ARM SoCs). The Windows-only static counters are now wrapped in `#ifdef _WIN32`; Linux gets its own `static unsigned long long last_cpu_total/last_cpu_idle` for delta tracking.
- **Detection lesson**: Stub branches that `return 0` for monitoring fields look indistinguishable from a healthy idle system. Either implement, or return `NaN`/sentinel that surfaces "not available" to dashboards.

### [FIXED] BUG-PTZ-01 — `deletePreset` returned false for every protocol
- **File**: `cpp-backend/src/core/ptz_manager.cpp:217` (pre-fix)
- **Bug**: Stub `return false` while `savePreset` had a real Hikvision ISAPI implementation. Frontend "delete preset" calls succeeded over HTTP (controller returned 200) but the camera kept the preset → "stale presets that won't go away" UX.
- **Fix**: Per-protocol implementation matching the existing save/move pattern: HIK ISAPI `DELETE /ISAPI/PTZCtrl/channels/1/presets/<id>`, Dahua CGI `GET /cgi-bin/ptz.cgi?action=clearPreset&channel=0&arg1=<id>`. Added `deleteDigest()` helper to `http_digest_client.h` (passes through `requestDigest`'s generic-method path) and `httpDelete()` to PTZManager. ONVIF stays unimplemented (RemovePreset SOAP envelope not built yet; we WARN explicitly so the UI can route around).
- **Detection lesson**: When `savePreset` is per-protocol but `deletePreset` returns `false`, that's an asymmetry signal — the feature is half-wired. Audit "set/get/delete" triples for missing implementations the same way we audit lock/unlock symmetry.

### [FIXED] BUG-ALERT-01 — Email/SMS/AlarmOutput were log-only stubs
- **File**: `cpp-backend/src/events/alert_router.cpp:579/589/686` (pre-fix)
- **Bug**: Three core notification channels never actually sent anything. Any rule with EMAIL/SMS/ALARM_OUTPUT channels silently logged "→ user@x" with no delivery. Operators could believe alerts were going out when they weren't.
- **Fix**: SMTP via libcurl (`smtp[s]://`, STARTTLS for plain), Twilio HTTPS for SMS, configurable HTTPS POST for alarm relay. All routed through the existing `webhookRunner` (bounded queue 128, 2 workers, drops on overflow). Header injection guarded by `sanitizeHeader` (strips CR/LF). Body capped at 8 KB to prevent oversized-event blowup. `shutting_down` checked at job entry to avoid use-after-free during process exit.
- **Detection lesson**: Log-only stubs in production-named functions (`sendEmail`, `sendSMS`) are an operational lie. Rename to `sendEmail_NotImplemented` or guard with a runtime assert until wired.
