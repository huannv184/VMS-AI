# Architectural Decisions — AI Camera System

## 2026-05-07 Shared SMTP helper — `vms::utils::sendEmailAsync`

### Decision: Extract SMTP transport into `cpp-backend/src/utils/email_sender.cpp`; both AlertManager (legacy) and AlertRouter (new) call it.
- **Choice**: New `vms::utils::sendEmailAsync(subject, body, recipients, source_label)` plus exposed `sanitizeMailHeader(s, max_len)`. Internal `BackgroundJobRunner("email-sender", 2 workers, queue=128)` is shared by all callers. AlertRouter::sendEmail loses ~80 lines of inline libcurl + queue plumbing; AlertManager::sendEmail goes from a MOCK `LOG_INFO` to a real send by simply composing subject/body and delegating. SMS in AlertRouter keeps a thin local `sanitizeHeader` alias to the helper to avoid touching the Twilio call site for this refactor.
- **Rationale**: Fixing BUG-ALERT-02 (legacy email mock) the obvious way — copy the AlertRouter SMTP body into AlertManager — would have left two parallel implementations of the same risky transport. Next operational change to SMTP (e.g. adding Bcc, switching to OAuth2 SASL, raising the body cap) would have to be done in two places, and the next person who skipped one would re-introduce the divergence. One transport, two policy layers (rule.email_addresses comes from RuleEngine; recipient comes from `alert_rules` table) is the right factoring.
- **Trade-off**: One extra header + one extra TU. Header is included by both alert_router.cpp and alert_manager.cpp, so a touch causes a 2-file recompile. Acceptable. The shared `BackgroundJobRunner` instance means one stuck SMTP host can backlog email sends from BOTH paths — that's actually correct behaviour (the hosts and the bottleneck are shared) and the drop-on-full bound prevents unbounded growth.
- **Concurrency invariant**: Helper composes the message + reads DB settings on the calling thread (so a DbManager destruction during shutdown can't race the queued job's settings read), then submits the libcurl work to the runner. The runner's worker checks `vms::core::shutting_down` at job entry to avoid use-after-free during process exit. Same pattern AlertRouter already uses for SMS/AlarmRelay.
- **Why not delete legacy AlertManager outright**: It's still wired through `EventManager::processEvent` → `AlertManager::processEvent` → reads `alert_rules` table → CRUD'd via `alert_controller.cpp` REST. Frontend Settings page still exposes it. Removing requires a DB migration + UI change + customer comms. This refactor is a strict superset (legacy now actually delivers email) so deleting can stay deferred.

### Decision: Move `AlertManager::sendWebhook` off the EventManager broadcast loop.
- **Choice**: New private `webhookRunner()` (anonymous-namespace function-static `BackgroundJobRunner("alert-mgr-webhooks", 2, 128)`). `sendWebhook` builds the JSON payload + URL copy on the calling thread, then submits a job that holds copies (not references) of all needed values. Job sets `CURLOPT_TIMEOUT=15s`, `CURLOPT_CONNECTTIMEOUT=10s`, `NOSIGNAL=1`, and bails on `vms::core::shutting_down`.
- **Rationale**: Pre-fix, one slow/dead webhook host blocked the entire EventManager broadcast loop for libcurl's OS-default ~120s. With AlertManager dispatch already running under `rules_mutex_`, a single dead webhook also serialised every subsequent event evaluation across all rules. Same severity class as BUG-ALERT-01 — fixing one path without the other was incomplete.
- **Trade-off**: Two BackgroundJobRunner instances now live in the legacy path (`alert-mgr-webhooks` + the shared `email-sender`). Could be consolidated, but the bottleneck profiles are different (webhooks tend to be high-rate small POSTs; SMTP tends to be low-rate larger sends with TLS handshake) so separating them avoids one starving the other.

### Decision: Don't delete `AlertRouter`'s SMS `sanitizeHeader` (alias instead of inline removal)
- **Choice**: After moving SMTP composition out of AlertRouter, the `sanitizeHeader` symbol in AlertRouter's anonymous namespace becomes a 1-line `inline` alias to `vms::utils::sanitizeMailHeader`. SMS body + recipient sanitisation keeps using the local name.
- **Rationale**: Renaming all SMS call sites to use the new helper would have ballooned the diff with no behavioural change. The alias is free at runtime (constexpr inline) and keeps the SMS code reviewable as a self-contained block.
- **Trade-off**: One trivial duplicate symbol. If we later refactor SMS too, drop the alias.

---

## 2026-05-07 MediaPipeline extraction — group BufferPipeline + ContinuousRecorder + MediaMtxPublisher

### Decision: Introduce `vms::core::MediaPipeline` (not a singleton; one per camera, owned by PipelineContext)
- **Choice**: New `cpp-backend/include/core/media_pipeline.h` + `src/core/media_pipeline.cpp`. Class aggregates the 3 per-camera media subsystems (BufferPipeline, ContinuousRecorder, MediaMtxPublisher) behind a 3-phase start API: `startBuffer()` (Phase 1, before worker), `startMediaMtx()` (Phase 2, after worker construction, before worker->start), `startContinuousRecorder()` (Phase 3, after worker->start + 500ms staggered sleep). RAII destructor enforces the legacy stop ordering buffer → continuous_recorder → mediamtx. PipelineContext now holds a single `unique_ptr<MediaPipeline> media` instead of 3 raw unique_ptrs.
- **Rationale**: Phase A memory deferred this as a "large refactor creating new abstraction layers". After re-reading the code, the actual scope was: 6 lifecycle touchpoints (3 starts + 3 destructor-stops) and 2 hot-path/API touchpoints (writeRawData inside rawPacketReady; triggerEvent in triggerEventRecording). Pulling them into one place removes ~50 lines from CameraPipelineManager (most of it the QObject::invokeMethod dance for moveToThread + start-on-Qt-thread on the publisher), localises future cleanup-vs-shutdown ordering changes, and makes "add a 4th media output" a one-file edit.
- **Trade-off**: One extra header + one extra TU. The 3-phase API breaks the RAII single-call ideal — the alternative was a single `start()` that took a callback to interleave worker setup, but that's more confusing than 3 named phases. The 500ms staggered sleep stays in CameraPipelineManager between Phase 2 and Phase 3 (where the worker actually starts) rather than baked into MediaPipeline; the sleep is the manager's concern (worker-startup ordering), not the media subsystem's.
- **Concurrency invariant preserved**: rawPacketReady lambda still acquires the manager's `mutex_` in shared mode for the pipelines_ lookup and only then calls `ctx->media->writeRawData(...)`. MediaPipeline does NOT take any of the manager's locks; BufferPipeline's internal synchronisation is what serialises concurrent writers.
- **Single-source-of-truth note**: ContinuousRecorder pulls its own RTSP and is independent of the rawPacket fan-out — keeping it inside MediaPipeline groups it by *purpose* (media output) rather than by *data dependency*. That feels right — ops thinks "what does this camera produce?" not "where does each subsystem read from?".

### Decision: Lifecycle logging moves with the subsystem (MediaPipeline owns the success/disabled/failed log lines)
- **Choice**: `[MediaPipeline] Camera N MediaMTX publisher started/disabled/failed` log lines now emit from inside MediaPipeline::startMediaMtx(); CameraPipelineManager no longer mirrors them.
- **Rationale**: Decisions about what to log live next to the code that owns the state. Pre-extraction, the manager logged because it was the only place that knew the state; post-extraction, MediaPipeline knows. Mirroring in both places leaks abstraction boundaries and risks drift.
- **Trade-off**: One extra logger import in MediaPipeline.cpp. Acceptable.

---

## 2026-05-06 Phase B refactor — PipelineStateStore as single source of truth

### Decision: Remove dual-write of latest frame/objects/metadata between PipelineContext and PipelineStateStore
- **Choice**: Drop the `cv::Mat latest_frame`, `std::vector<TrackedObject> latest_objects`, `nlohmann::json last_metadata_json_` fields and their two sub-mutexes (`frame_mutex`, `objects_mutex`) from `PipelineContext`. Move the lone consumer that read `ctx->latest_objects` (the `rawPacketReady` H264 broadcast lambda in `camera_pipeline_manager.cpp`) to read from `PipelineStateStore::latestObjects(camera_id)` instead. PipelineStateStore now has zero readers that bypass it; its `updateFrame` / `updateMetadata` calls are the single authoritative writes.
- **Rationale**: The Phase A audit (`phase_a_refactor_2026_05_01.md`) flagged this dual-write but deferred it because the audit said removing the context cache requires touching ~30 read sites. Re-reading the code today, all three public getters (`getLatestFrame`, `getLatestObjectsJson`, `getLatestMetadata`) had already been migrated to read from PipelineStateStore — only one internal reader remained. So the actual cost was ~5 line changes across one file, not 30. Single-source-of-truth removes the "which copy is authoritative when they disagree?" hazard and saves two mutex acquires + one `cv::Mat::clone()` (~691KB) per real-frame on the hot path.
- **Trade-off**: The `rawPacketReady` lambda now takes the store's `shared_mutex` in shared mode for a vector copy, instead of the context's `std::mutex` exclusive. Slightly cheaper under read concurrency. Sequencing-wise, `handleFrameProcessed` writes the store BEFORE the H264 broadcast lambda might fire on the next packet, so the lambda sees objects from the most-recently-processed frame — same semantics as before.
- **Failure mode considered**: If PipelineStateStore::latestObjects returns an empty vector while ctx had cached an older copy, the H264 broadcast for one keyframe might attach an empty objects array. Acceptable: the previous behavior also showed stale-or-empty objects depending on timing, and the next decoded frame catches up within ~33ms at 30fps.

### Decision: Restart backoff lives in NativeReaderWorker; HealthMonitor does NOT carry backoff state
- **Choice**: Delete the unused `restart_backoff_count_` + `next_restart_allowed_` maps from `CameraPipelineManager`, the `restart_backoff_count` field from `CameraHealthSnapshot`, and `HealthMonitor::setRestartBackoff()`. Reconnect/backoff is owned end-to-end by `NativeReaderWorker::run()` (local `reconnect_count` + `backoffDelayMs(...)` per-stream); no other module reads or writes the count.
- **Rationale**: Phase A memory deferred a "migrate restart_backoff to HealthMonitor" task assuming the maps would eventually inform restart policy. After re-grep'ing today, the entire HealthMonitor backoff API was call-free for ≥5 days, and the only actual restart logic lives inside the worker thread that performs the reconnect — there is no sensible policy decision a centralized backoff sink could drive that the worker isn't already driving locally. Conclusion: there's nothing to migrate; delete the dead surface so nobody else mistakes it for a contract.
- **Trade-off**: If a future feature needs cross-module visibility of backoff state (e.g. a "/api/health" endpoint showing per-camera retry counts), it'll have to either (a) plumb the worker's count through `setState`/`updateFrameHeartbeat` patterns, or (b) reintroduce a sink. Acceptable — both options are 10-line changes if/when needed.

---

## 2026-05-06 HTTP server startup is fail-fast via future + wait_for_server_start

### Decision: Use `app_.run_async()` + `app_.wait_for_server_start()` instead of `std::thread([&]{ run(); })` + sleep
- **Choice**: `HttpServer::runAsync()` wraps `crow::App::run_async()` and returns `std::future<void>`. `main.cpp` keeps the future alive and gates on two checks: (1) `app_.wait_for_server_start(5s) == no_timeout` confirms the accept loop is up; (2) `future.wait_for(0)` immediately before announcing "Backend started successfully" — if ready, the server already exited. Either signal triggers `future.get()` (re-throws bind/route exceptions) and the catch block in `main()` does the LOG_ERROR + early return. No more "started successfully" log when port isn't actually listening.
- **Rationale**: BUG-HTTP-01 (duplicate CROW_ROUTE, port 8000 never binds) was triggered in production and the silent failure mode added 30+ minutes of debugging because the smoke-test message — "Backend started successfully" — was a lie. The class of failure (any throw from `app.run()` after std::thread launch) is permanent; the previous mitigation (just LOG_ERROR inside the thread lambda) buried the signal at WARN-level surrounded by 200+ unrelated startup lines. Crow itself already exposes the right primitives (`run_async` and the `server_started_` condvar via `wait_for_server_start`); we just weren't using them.
- **Trade-off**: 5-second startup timeout — chose 5s because `serverConfig.threads` defaults to hardware concurrency × 1 (16+ on dev boxes) and Crow per-thread io_service init under PCH/Release config measured ~200ms on first run. If this timeout proves too aggressive on slow disks (cold cache + SSL cert load), bump to 10s; if too lax, lower to 2s. Future poll is `wait_for(0)`, not `wait_for(short timeout)`, because at the announce point we already passed the 5s wait — any async death since then has already populated the future.
- **Alternative considered**: Inline `app_.run()` on the main thread (no async at all). Rejected because the rest of bring-up (storage init, camera load, websocket bring-up) needs to run while Crow is also accepting; Qt event loop is on the main thread and Crow's `run()` is blocking.

---

## 2026-05-04 Night-shift bucketing for attendance rollup

### Decision: Bucket attendance punches by `shift_date`, not calendar date
- **Choice**: `queryAttendanceForDate(YYYY-MM-DD)` widens its SQL window to `[day_start-12h, day_start+36h)`, drops `GROUP BY` from SQL, and aggregates in C++ keyed on `(person_id, employee_id, shift_date_midnight)`. `shift_date` for an overnight shift (`start_min > end_min`, e.g. 22:00→06:00) is determined by the midpoint-of-off-duty rule: punches whose minute-of-day < `(end_min + start_min) / 2 + ...` (i.e. < 14:00 for 22→06) are credited to yesterday's instance; ≥ midpoint to today's.
- **Rationale**: The legacy SQL `GROUP BY a.person_id, ...` keyed on `MIN(a.timestamp)` within `[day, day+24h)` produced two split rows for one night-shift instance (IN at 22:00 on day N, OUT at 06:00 on day N+1) AND a wrong `late_minutes` (06:05's `min_of_day=365 < shift_min=1320` ⇒ delta negative ⇒ silently "on time"). Doing the bucketing in C++ keeps a single code path across SQLite and Postgres, preserves the existing API shape (`?date=...`), and lets day-shift workers pass through unchanged.
- **Trade-off**: SQL window doubles (24h → 48h) so the query scans more rows when queried for a single day; aggregation moves from SQL to C++ map ops. For typical attendance volume (<10 punches/person/day) this is well below 1ms overhead. The wider window relies on an index on `attendance_events.timestamp` to stay efficient — already present.
- **Why the midpoint rule (not "after midnight = yesterday")**: Robust against late check-outs (worker punches OUT at 09:00 → still belongs to yesterday's 22:00→06:00 instance) and early arrivals (21:00 IN → today's instance). A simple "hour < end_hour" rule would mis-bucket the 06:05 sharp boundary case.

### Decision: Late-minute computation uses epoch-delta from `shift_date_midnight`, not minute-of-day
- **Choice**: `lateMinutesForPunch(ci_ts, shift_date_midnight, start_min, grace) = max(0, ((ci_ts - shift_date_midnight)/60) - start_min - grace)`.
- **Rationale**: Subtracting `shift_date_midnight` (epoch sec of the shift instance's start day) from `ci_ts` yields minutes since shift_date 00:00 — which can exceed 1440 if the only punch landed on the next calendar day. This is the only formulation that gets "missed-the-IN, only punched at 06:05" correctly attributed as "485 min late" instead of "on time by 955 min".
- **Trade-off**: One additional helper, but unit-tested in `test_attendance_shift.cpp` (inline reproduction).

---

## 2026-05-02 Default-password gate + RBAC parity

### Decision: Use a JWT `token_use="password_change_pending"` claim rather than a "limited access token"
- **Choice**: When admin is on the factory-default password, login returns *only* a 5-min JWT with `token_use="password_change_pending"`. AuthMiddleware's access-token verifier already requires `token_use="access"` (or empty) so this token is rejected by every other API. The single endpoint that accepts it (`POST /api/auth/change-password-on-login`) verifies the purpose claim explicitly via `verifyPasswordChangeTempTokenJwt`.
- **Rationale**: Reuses the existing `2fa_pending` token-purpose mechanism instead of inventing a stateful flag-on-user-row approach (which would need DB writes on every login). Same pattern as the working 2FA flow.
- **Trade-off**: Adds one new JWT purpose. Acceptable — the verifier helper is 8 lines and shares the existing `signUserToken/verifyJwtForUse` plumbing.

### Decision: NVR reboot requires `SYSTEM_ADMIN`, not `DEVICE_WRITE`
- **Choice**: Three-tier gate inside the device module — `DEVICE_READ` (list/info/channels), `DEVICE_WRITE` (create/update/delete/sync), `SYSTEM_ADMIN` (reboot only).
- **Rationale**: Reboot is irreversible from the API caller's perspective and can interrupt a 24x7 recording. Operator role (role_id=2) holds DEVICE_WRITE, so they can configure cameras during a shift, but only admin can physically restart the NVR. Matches the "operator: day-to-day; admin: dangerous ops" split already used elsewhere in `rolePermissions()`.
- **Trade-off**: A site that wants 24x7 NOC operators with reboot rights has to give them admin role. Acceptable for current customer profile; revisit if a customer asks.

### Decision: Audit log redaction list — explicit allowlist of sensitive setting keys
- **Choice**: `static const std::unordered_set<std::string> sensitive_keys` in the settings PUT handler — `smtp_pass`, `twilio_auth_token`, `alarm_output_token`, `ldap_bind_password`, `openai_api_key`, `smb_password`. Anything in the set logs "(value redacted)" instead of "old → new".
- **Rationale**: An audit table that records "smtp_pass: 'oldpass' → 'newpass'" is itself a credentials store. Allowlist (rather than denylist) means a new sensitive key has to be explicitly registered — defaulting to "log the value" is the wrong fail-safe direction.
- **Trade-off**: A sensitive key not in the set will leak. Mitigated by code review at the PR level; if this proves error-prone, switch to a single `is_sensitive_setting(key)` predicate that defaults to true and explicitly opts in non-sensitive keys.

---

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

---

## 2026-05-01 Phase A: FrameBus / HealthMonitor refactor preparation

### Decision: `FrameEnvelope::raw_frame` is `std::shared_ptr<const cv::Mat>`, not `cv::Mat`
- **Choice**: Replace `cv::Mat raw_frame` field with `std::shared_ptr<const cv::Mat>`. Producer constructs via `std::make_shared<const cv::Mat>(frame)` (header-copy + refcount++ on the pixel buffer, no deep copy). Old code used `frame.clone()` which was a full ~691 KB pixel copy per frame per camera with subscriber.
- **Rationale**: The bus must support N consumers with O(1) fan-out cost. Even one slow async consumer holding onto the frame must not force a deep copy on the producer side. `shared_ptr` makes ownership explicit and survives Phase B when an async-queue consumer is added; cv::Mat's mutable internal refcount is not enough on its own to reason about lifetime across threads.
- **Trade-off**: Heap-allocates a `cv::Mat` header (~96 B on x64) per published frame instead of using a stack-local. Negligible vs. the 690 KB deep-copy cost it removes. Verified by `FrameBus.RawFrameIsSharedZeroCopyAcrossConsumers` (pointer identity across 3 consumers).

### Decision: `FrameBus::compactExpiredLocked` removed from `publish`/`publishStateChange`/`subscriberCount`
- **Choice**: Compaction of expired weak_ptr slots only happens in `subscribe()` (rare path). Hot paths skip expired slots implicitly via `weak_ptr::lock()` returning null.
- **Rationale**: Compaction under the bus mutex on every publish is a write under what should be a read-mostly fan-out path. With 3 cameras × 15 fps the previous code took the lock + linear scan + erase ~45 times/s on top of every publish. Cost was small in absolute terms but unprincipled.
- **Trade-off**: Expired slots accumulate in the vector between subscribe calls. Worst case is bounded by "consumers that died without explicit unsubscribe between two subscribe events" — in our use we always pair attach/detach in pipeline lifecycle, so this stays small. If we ever see growth in production, add a counter-based compact every N publishes.

### Decision: `HealthMonitor::start`/`stop` lifecycle is mutex-guarded; tick callback is snapshot-then-invoke
- **Choice**: New `std::mutex lifecycle_mutex_` guards `timer_` and `tick_callback_`. `start()` is idempotent — if `timer_` is already non-null it returns instead of overwriting `tick_callback_` (which would race with the firing tick lambda). `stop()` clears both members under the lock, then performs the QTimer disposal outside the lock. The tick lambda copies `tick_callback_` under the lock and invokes the copy outside the lock so user callbacks (e.g. `globalWatchdogTick`) cannot serialize with `start`/`stop`.
- **Rationale**: The pre-fix code allowed: (a) two threads racing `start()` to overwrite the callback while the timer is firing, (b) `stop()` setting `timer_=nullptr` while another thread checks `if (timer_)` in `start()`, and (c) the tick lambda reading `tick_callback_` lock-free while `stop()` clears it. None of these have triggered in production yet (only the singleton manager touches HealthMonitor today), but per `core.md` we don't ship code with avoidable concurrent UB.
- **Trade-off**: One extra mutex acquire per timer tick (1/sec) — sub-microsecond cost vs. correctness.

### Decision: `FrameBusDiagnostics` is the first real consumer; subscribed in pipeline lifecycle
- **Choice**: New `vms::core::FrameBusDiagnostics` singleton implements `IFrameConsumer`. `attach(camera_id)` is called from `CameraPipelineManager::startPipeline` immediately after `HealthMonitor::registerCamera`, and `detach(camera_id)` is called from both `cleanupPipeline` and the bulk path in `stopAllPipelines`. Singleton uses a static null-deleter `shared_ptr<IFrameConsumer>` self-ref to satisfy the bus's weak_ptr ownership model without tying lifetime to any caller.
- **Rationale**: Without a real subscriber, the FrameBus contract is "scaffolding that compiles but has never run end-to-end." The diagnostics consumer is intentionally minimal (one relaxed atomic increment + a 30-second-throttled log on the hot path) so it can run in production permanently and serve as a metrics tap (`framesFor(camera_id)`, `totalFrames()`) for later observability work.
- **Trade-off**: Adds one mutex acquire on the per-frame hot path (for the per-camera counter). At 15 fps × 3 cameras that's 45 lock acquires/s — negligible. If we ever hit real load (>200 fps aggregate) we'd switch the per-camera counter to a sharded atomic.

### Decision: Tests link the REAL `frame_bus.cpp`, not an inline reproduction
- **Choice**: `test_frame_bus.cpp` adds `${CMAKE_SOURCE_DIR}/src/core/frame_bus.cpp` and `src/utils/logger.cpp` directly to its target sources, breaking the project convention where most tests are inline reproductions.
- **Rationale**: FrameBus is now a load-bearing concurrency primitive. An inline reproduction can drift from the real impl silently — and concurrency bugs are exactly the class of issue an inline copy will miss. The cost (one extra spdlog link + Logger-shim TU) is small.
- **Trade-off**: Test-target compile time is a few seconds longer; tests now require spdlog on the link line. Documented in `tests/CMakeLists.txt`.

### Decision: ctest on Windows must inject vcpkg DLL path into PATH
- **Choice**: `set_tests_properties(... PROPERTIES ENVIRONMENT "PATH=<build>/$<CONFIG>;<vcpkg>/bin;$ENV{PATH}")` for every test target.
- **Rationale**: Modular OpenCV DLLs (`opencv_core4.dll`, `opencv_imgproc4.dll`, ...) live in `vcpkg_installed/x64-windows/bin` and are NOT auto-copied next to test executables (only Qt/Boost/FFmpeg are). Without this, `ctest` exits with `0xC0000135` (STATUS_DLL_NOT_FOUND) the moment it tries to launch a test that touches OpenCV, regardless of the developer's interactive shell PATH.
- **Trade-off**: Adds a hard-coded triplet (`x64-windows`) in the test CMake. If we ever cross-compile or change triplet, this will need to be parameterized — but that's a one-line change.
