# Past Bugs — AI Camera System

## 2026-05-15 BUG-WS-CAMERA-NO-RBAC-01 closed — WS subscribe now enforces allowed_cameras

- **Files**: `cpp-backend/src/streaming/camera_stream_manager.cpp` (+`<database/camera_repository.h>` include, +file-scope per-socket allowed_cameras map + mutex, populate at AUTH success via `CameraRepository::getFilteredCameras`, check at subscribe, cleanup at socketDisconnected).
- **Bug**: WS subscribe accepted any `camera_id` from any authed client and called `addClient(cam_id, socket)`. The `permissions` table HAS `allowed_cameras` column and `PermissionRepository::getPermissions` returns it; `CameraRepository::getFilteredCameras(userId)` does the admin-bypass + camera+site merge. But the WS layer had ZERO calls into either. A logged-in viewer-role user subscribed via WS to any camera_id and received its frame stream (raw JPEG / H.264 NALs / fmp4 fragments / AI events / rule-fire alerts). Real PII / authorisation leak in multi-tenant deployments where operators had configured per-user camera scopes.
- **Detection**: documented HIGH in the 2026-05-15 WS audit memo (`BUG-WS-CAMERA-NO-RBAC-01`), then fixed in this commit as the Tier 1 priority item per the project-wide review.
- **Fix**:
  1. Per-socket allowed-camera cache in `camera_stream_manager.cpp` namespace scope: `std::unordered_map<QWebSocket*, std::unordered_set<int>>` + `std::mutex`. Keyed by raw `QWebSocket*` — pointer is stable for the socket's lifetime (Qt::deleteLater fires after socketDisconnected returns, so the pointer is valid for the map erase).
  2. At AUTH success: if `user.role_id == 1` (admin) → set `vms_is_admin=true` Qt property, skip the map. Else: call `CameraRepository::getFilteredCameras(user.id)` (admin-bypass + allowed_cameras + allowed_sites merge already implemented there for HTTP `/api/cameras` filtering), extract camera IDs into the set, store under the mutex. One DB query per AUTH, NOT per subscribe.
  3. At subscribe: `vms_is_admin=true` OR `camera_id == 0` (global) bypass. Else look up the set, reject if camera_id ∉ set with a structured `{type:"subscribe_denied", reason:"camera_not_in_user_scope"}` message + throttled WARN log carrying user_id and camera_id for operator visibility.
  4. Auth-disabled mode (`config.auth.enabled=false`) treats the socket as admin so subscribe RBAC short-circuits — matches existing "no auth means no filter" semantics elsewhere in the codebase.
  5. At socketDisconnected: erase the map entry before `deleteLater`.
- **Concurrency notes**: the map mutex is held only briefly (one insert/erase/lookup). AUTH path does the DB query OUTSIDE the mutex (one DB hit per WS connection — once-per-session, not hot). Subscribe path's lookup is `O(1)` average via unordered_set. Operator visibility WARN is throttled at 5s to prevent log floods from a probing client.
- **What's NOT closed in this pass** (documented as BUG-WS-GLOBAL-FANOUT-01):
  - Camera 0 (global) is allowed for any authed user. The `EventManager::createEvent` path broadcasts every event to both `broadcastEvent(camera_id, …)` AND `broadcastEvent(0, …)`. A viewer subscribed to camera 0 still receives events whose camera_id is outside their scope. The fix is broadcast-side filtering: when fanning a non-zero camera_id event out to camera-0 subscribers, check each recipient's allowed-set and skip if the event's camera_id isn't there. That touches `broadcastEvent` / `broadcastH264Frame` / `broadcastFmp4Fragment` / `broadcastRawFrame` and needs the same map lookup. Out of scope here because it changes the broadcast loop's complexity; can land as a follow-up once we confirm operators do use the global channel.
- **Detection lesson**: when a DB schema column exists with a clear policy intent (`allowed_cameras`), grep for its consumers BEFORE assuming the policy is enforced. Schema is documentation of intent, not evidence of enforcement. Pair this with the 2026-05-14 "facade with zero callers" pattern — any DB column with a single producer (admin UI POST) and zero readers in enforcement code is a latent leak.

## 2026-05-15 BUG-WS-PREAUTH-NOTIMEOUT-01 + WS-CAMERA-NO-RBAC-01 — WebSocket auth/session audit

- **Files**: `cpp-backend/src/streaming/camera_stream_manager.cpp` (+`<QTimer>` include, 10s pre-auth timeout in `onNewConnection`).
- **BUG-WS-PREAUTH-NOTIMEOUT-01 (HIGH)**: `onNewConnection` accepted a `QWebSocket`, attached message handlers, and set `vms_authed=false`. The socket then sat idle until either (a) the client sent something or (b) the client disconnected. There was no timer to close the connection if it stayed unauthenticated. An attacker could open N TCP+WS handshakes, never send the AUTH frame, and hold N file descriptors + per-connection QWebSocket buffers indefinitely. The `auth_enabled=false` operator escape doesn't fix this — even there, the socket stays half-open until the first message of any kind. Fix: `QTimer::singleShot(10000)` parented to the socket; on fire, if `vms_authed` is still false, close with `CloseCodePolicyViolated`. Timer auto-deletes with the socket via Qt parent ownership; no manual `stop()` on AUTH success needed because the lambda no-ops on `authed=true`. 10s is generous (typical browser+JS does <1s from connect to AUTH frame).
- **BUG-WS-CAMERA-NO-RBAC-01 (HIGH, NOT fixed in this pass — documented for next pass)**: `processTextMessage` line 240-243 accepts ANY `camera_id` from any authed client and adds them to `camera_clients_[camera_id]`. The `permissions` table HAS an `allowed_cameras` column (db_manager.cpp:735) and `PermissionRepository::getPermissions` returns it (permission_repository.h:28), but `grep -rn "allowed_cameras"` shows ZERO consumers in policy-enforcement code. A logged-in viewer-role user can subscribe to any camera_id and receive its WS frame stream (raw JPEG, H.264, fmp4, plus all rule-fire events). This is a real PII / authorisation leak — the schema implies operators have configured per-camera viewer scopes, but the WS layer ignores them. The HTTP `/api/cameras/*` endpoints also need verification. Right shape: load `allowed_cameras` for the authed user at AUTH time (or look up lazily on subscribe with caching), reject subscribe if camera_id ∉ allowed set OR if allowed_cameras is empty AND the user lacks a "view-all-cameras" permission. Out of scope for this commit because it touches the auth lookup path + caching + the broadcast filter.
- **Other findings documented** (not fixed in this pass, see `decisions.md`):
  - **BUG-WS-PROXY-IPBIND-BYPASS-01 (MED)**: X-Forwarded-For from `/api/ws/ticket` + the localhost-bypass in `decodeWsTicketJwt` combine to defeat IP binding behind a reverse proxy. Proxy connects to backend from localhost → ticket binds to forwarded IP, but the WS connect lands as "localhost" → localhost exception skips IP check. Need a trusted_proxies config list.
  - **BUG-WS-NO-CONN-CAP-01 (MED)**: no per-IP / global connection count limit. Combined with no pre-auth timeout this was a clean DoS vector; the timeout fix above closes the immediate attack but doesn't stop a client from immediately reopening.
  - **BUG-WS-NO-MSGSIZE-CAP-01 (LOW-MED)**: text/binary message size relies on Qt's `QWebSocket::maxAllowedIncomingMessageSize` default (40 MB last I checked). Should be tightened per `config.websocket.max_message_size_mb` (already in config) — currently the config field is parsed but not applied.
  - **BUG-WS-SHUTDOWN-NEWCONN-01 (LOW)**: `vms::core::shutting_down` not consulted in `onNewConnection`. During graceful shutdown, new connections are still accepted briefly until `stop()` closes the server.
  - **BUG-WS-NO-METRICS-01 (LOW)**: no observability for connection count / pre-auth count / AUTH success-fail counters / disconnects. Mirror of the `delivery` + `batch_writer` counter pattern landed earlier today.
- **Detection lesson**: the AUTH state machine was correct (ticket signing, JTI replay protection, single-use enforcement, IP binding with localhost exception, 30s ticket TTL) but missed the resource-lifetime axis. Authentication design ≠ session resource policy. Audit every long-lived connection for "what bounds an unauthenticated lifetime?".

## 2026-05-15 BUG-PSS-LOCKHELD-MEMCPY-01 — PipelineStateStore audit

- **Files**: `cpp-backend/include/core/pipeline_state_store.h` (`latest_frame_jpeg` + `latest_objects` types changed to `std::shared_ptr<const ...>`, +`latestFrameJpegShared()` decl), `cpp-backend/src/core/pipeline_state_store.cpp` (updateFrame builds shared_ptrs pre-lock, swaps pointers under lock; readers grab refcounted handle under shared_lock then deep-copy out for legacy API or return handle directly for zero-copy callers).
- **Bug**: `PipelineStateStore::updateFrame` held a `std::unique_lock<std::shared_mutex>` for the entire 200–500 KB JPEG `vector::assign`, the `vector<TrackedObject>` copy, AND the `nlohmann::json` copy. Called at 30 fps per camera. At 50 cameras × 30 fps = 1500 writes/sec each holding the single global mutex for hundreds of microseconds × the memcpy cost. Every reader on every camera (`/api/cameras/<id>/snapshot.jpg`, stats endpoint, WS H.264 keyframe broadcast that calls `latestObjects()`) competed for the same mutex.
- **Detection**: load + concurrency audit per `.claude/rules/load-test.md` & `core.md` ("100x current usage"). Phase B refactor (2026-05-06) made PipelineStateStore the single source of truth; with one global mutex across N cameras, even a small write held that mutex through a memcpy of 100s of KB. Lock contention model: read on camera 7 blocks because write on camera 23 is mid-memcpy.
- **Fix**: store the JPEG bytes and objects vector as `std::shared_ptr<const std::vector<...>>`. Writer builds the immutable copy OUTSIDE the unique_lock; lock hold time drops to a pointer assignment (~10s of ns). Readers take a `std::shared_lock`, copy the refcounted handle (O(1) bump), drop the lock, then deep-copy out for the legacy `latestFrameJpeg() → vector<char>` API or return the handle directly via the new `latestFrameJpegShared()` API. The underlying buffer stays alive as long as any reader holds the handle, even if a newer frame replaces it in the store.
- **Concurrency notes**: `std::make_shared` allocates a control block + buffer per frame. At 1500 fps that's measurable allocator churn — `mimalloc`/`tcmalloc` handle it fine, but a pool of pre-allocated buffers would amortise further. Out of scope for this pass. Reader path is wait-free w.r.t. the writer: shared_lock parallelises with other shared_locks, and the writer's unique_lock now only blocks readers for the pointer-swap window (~ns). On Windows MSVC, `std::shared_ptr` copy is atomic via interlocked ops — no further synchronisation needed.
- **Backward compat**: `latestFrameJpeg()` signature unchanged (`std::optional<std::vector<char>>` return). Existing callers in `camera_pipeline_manager.cpp` keep working — they continue to get a deep copy on read, but that copy now happens after the lock is released. The hot read paths (HTTP snapshot endpoint, WS keyframe broadcast) can be migrated to `latestFrameJpegShared()` separately for zero-copy.
- **Deferred findings from same audit pass** (documented in `decisions.md`):
  - Single global mutex serializes ALL cameras — should be sharded per-camera (bucket-by-camera_id or per-snapshot mutex). Bigger surgery; needs design pass.
  - `snapshot()` returns the whole `PipelineStateSnapshot` by value (now cheap for jpeg+objects since they're shared_ptrs, but still copies metadata json + `last_error` string). Could return shared_ptr<const Snapshot>.
  - `getOrCreateLocked` silently default-creates a map entry on operator[] — a stale producer (e.g. ai_worker still running after `removeCamera`) can resurrect the removed entry. No `isRegistered` gate.
  - No shutdown gate (`vms::core::shutting_down` not consulted). Singleton relies on static-destruction-order plus thread join before exit; consistent with pattern elsewhere but worth a memo.
  - `setState` heuristic `is_running = (state != FAILED)` — `CONNECTING` and `STOPPED` both set is_running=true, slightly counter-intuitive.

## 2026-05-15 BUG-DB-PG-BATCH-ABORT-01 + BATCH-VISIBILITY-01 — DbManager hot-path audit

- **Files**: `cpp-backend/include/database/db_manager.h` (+`BatchWriterStats` struct + 6 atomic counter fields + `batchWriterStats()` decl, `batch_queue_mutex_` marked `mutable`), `cpp-backend/src/database/db_manager.cpp` (counter wiring in enqueueEvent + flushEventBatch, SAVEPOINT around per-row INSERT on Postgres), `cpp-backend/src/api/event_engine_controller.cpp` (merge `batch_writer` block into `GET /api/rules/stats`).
- **BUG-DB-PG-BATCH-ABORT-01 (HIGH on Postgres deployments)**: `flushEventBatch` looped `query.exec()` per event inside a single `db.transaction()` and logged-but-continued on per-row failures. On Postgres MVCC, ANY statement error inside a transaction marks the tx as ABORTED — every subsequent INSERT fails with `current transaction is aborted, commands ignored until end`. Then `db.commit()` itself fails → the entire 50-event batch re-enqueues at the front of the queue → next flush hits the SAME poisoned row → infinite retry loop. The whole event ingestion pipeline halts behind one bad event. The existing `ON CONFLICT DO NOTHING` only covers the dupe-id case (BUG-DB-01); NULL constraint / type / CHECK violations all abort. SQLite tolerates this pattern (per-statement errors don't abort the transaction) so the bug only bites Postgres operators. Detection: code review of the fan-in path; no production hit yet because Events table schema is permissive (mostly nullable TEXT). Fix: SAVEPOINT before each INSERT on Postgres, ROLLBACK TO SAVEPOINT on failure (poisoned row is dropped — re-enqueueing it is precisely the loop we're avoiding), RELEASE SAVEPOINT on success. SQLite path unchanged. `row_failures_total_` atomic counter exposes the count.
- **BUG-DB-BATCH-VISIBILITY-01 (MEDIUM)**: same shape as BUG-ALERT-DROP-VISIBILITY-01 from the alert_delivery audit, this time on `event_queue_`. Pre-fix the only signal of drops was a throttled WARN log at 5s — operator could not answer "did we lose events overnight?". Six atomic counters added: `enqueued_total / dropped_total (queue-full + not-accepting) / flushed_total / flush_failures_total / row_failures_total / peak_queue_depth`. New `DbManager::BatchWriterStats` POD + `batchWriterStats()` wait-free accessor (atomic loads + try_lock for current_queue_depth so the operator dashboard can never contend the producer hot path). Exposed via `GET /api/rules/stats` under a `batch_writer` block (already ALERT_READ-gated).
- **Concurrency notes**: counters use relaxed memory order — the producer hot-path cost is one atomic increment per event (lock-amortised, already under `batch_queue_mutex_` for enqueue). Peak-depth uses a CAS retry loop after the push lock is released so the mutex hold stays at one queue mutation. The new `mutable` on `batch_queue_mutex_` allows `batchWriterStats() const` to take it via try_to_lock without breaking const-correctness.
- **Detection lesson** (DbManager audit, 2026-05-15): when a transactional batch loop logs-and-continues per-row failure, ask which engine you're on. Postgres semantics turn one bad row into a poisoned batch; SQLite tolerates it. Always SAVEPOINT around per-row inside a multi-row tx if Postgres is in the deployment set.
- **Deferred findings from same audit pass** (documented in decisions.md, not fixed in this push):
  - `transaction_mutex_` global serializer — 3 callers (ReID 60s flush, rule save, zone save) needlessly contend across per-thread connections. Removable but needs careful review.
  - SQLite WAL has no explicit `wal_checkpoint(TRUNCATE)` policy — under burst write + many concurrent readers, the WAL file can grow unbounded (PASSIVE auto-checkpoint never completes if any reader is mid-read).
  - `getSetting` prepares fresh statement per call — sub-ms but burst-amplified. Caching settings with a TTL (or in-memory snapshot refreshed on `setSetting`) is the right shape.
  - `registered_connections_` is push-only — short-lived threads leak connection entries until `close()`. Bounded for HTTP pool reuse, unbounded for ad-hoc `std::thread`.
  - `commit()` returns `void` from the public API — caller can't surface commit failures (engine auto-rolls back so no corruption, but TransactionGuard pattern silently treats failure as success).
  - WAL `journal_mode=WAL` pragma is re-set per thread connection (cheap, idempotent, but noisy).

## 2026-05-15 BUG-ALERT-DNS-PRODUCER-01 + DROP-VISIBILITY-01 + DB-PRODUCER-01 — alert_delivery hot-path audit

- **Files**: `cpp-backend/src/events/alert_delivery.cpp` (move SSRF + DB reads into worker, atomic submit/drop counters), `cpp-backend/include/utils/background_job_runner.h` (`+stats()` accessor with submitted/dropped/queue-depth counters), `cpp-backend/include/events/alert_delivery.h` (+`deliveryStats()` decl), `cpp-backend/src/api/event_engine_controller.cpp` (expose stats on `/api/rules/engine/stats`).
- **Bug 1 — DNS lookup on producer thread (HIGH)**: `deliverWebhook` and `deliverTelegram` called `vms::utils::isInternalUrl(url)` BEFORE submitting the libcurl job. `isInternalUrl` invokes blocking `getaddrinfo` (cpp-backend/src/utils/url_validator.cpp:168). The producer thread is one of: ZMQ bridge (single thread pulling from AI worker), brand event service worker (one per camera), AiEventProcessor worker pool, or HTTP request thread (manual `/api/events` POST). A slow DNS resolver (5+ sec on a dead webhook host) blocked the producer per event. **Worst case**: ZMQ bridge stalled → AI worker output queues up in the ZMQ socket → HWM exceeded → frames/events dropped at the AI worker side. Multiplier: at every event matching a webhook rule, not once-per-rule-URL — no DNS cache at this layer.
- **Bug 2 — Drop-on-full alerts are silent (MEDIUM)**: `BackgroundJobRunner::submit()` returns false when queue ≥ 128, only logs a throttled WARN every 5s. No counter, no metric, no operator-visible state. Burst load with a stalled webhook could drop tens of thousands of security alerts (intrusion, fire, smoke) per minute and the operator sees one log line per 5s. There's no way after the fact to know "how many alerts did we drop yesterday".
- **Bug 3 — DB settings reads on producer thread (LOW-MEDIUM)**: `deliverSMS` (3 settings: sid/token/from), `deliverTelegram` (2: bot_token/chat_id), `deliverAlarmOutput` (1: alarm_output_url) all called `DbManager::getSetting()` before submitting. Each call is a prepared SELECT on the `settings` table — sub-millisecond but real I/O on the hot path. At ~100 events/sec firing SMS+Telegram rules → 500 SELECTs/sec contending with the event batch writer's connection.
- **Detection**: load-mode audit per `.claude/rules/load-test.md` after merging the AlertManager consolidation (2026-05-14). The new `vms::events::deliverAction` is THE single dispatch path for every event in the system (AI worker via ZMQ, brand cameras via event service, REST API, holiday rollups). At 100× current event rate (~1000/sec burst from a 50-camera deployment) the producer-side blocking calls become the saturation point.
- **Fix**:
  1. **Producer side**: cheap validations only. Webhook URL prefix check (`http(s)://`) + non-empty. SMS recipients sanitised + filtered. Telegram chat_id resolved from metadata only (the DB fallback moved into worker).
  2. **Worker side**: DNS + DB reads run inside the job lambda. SSRF refusals still log + skip, just without burning producer time. Each channel's worker job is self-contained (URL + payload + bot_token captured by value).
  3. **Counters**: `BackgroundJobRunner` now tracks `submitted_total` / `dropped_total` (queue-full) / `current_queue_depth` as `std::atomic<uint64_t>`. New `BackgroundJobRunner::stats()` returns a snapshot struct. Exposed externally via `vms::events::deliveryStats()` → JSON; surfaced on the `/api/rules/engine/stats` endpoint (admin-only).
- **Concurrency notes**: producer-side check of `vms::core::shutting_down.load(memory_order_acquire)` retained as the early bail. Worker-side check before any heavy work (DB SELECT or curl_easy_perform) also retained. The new atomic counters are increment-only inside `submit()` under the queue mutex (counter writes piggyback on the lock; no extra synchronisation). `stats()` is wait-free (atomic loads only) — safe to call from any HTTP handler thread without contending the producer hot path.
- **What's NOT fixed in this pass (deferred)**:
  - **Pool sharing across channel types (HIGH)**: webhook + SMS + Telegram + alarm-output + UI fan-out all queue into the same 2-worker pool. A single stalled HTTPS endpoint blocks both workers for up to 15s, queueing OR dropping every other channel's deliveries. Right fix is per-channel-class pools (e.g. one "operator-controlled webhook" pool that's bigger, one "vendor API" pool for Twilio/Telegram). Punt — needs scope; logged in `decisions.md` as next-pass design.
  - **Per-rule URL DNS cache**: a rule pointing at one webhook re-resolves on every event. libcurl caches per-easy-handle but we create new handles per job. Adding a TTL'd `unordered_map<string, ResolveEntry>` at the alert_delivery layer would amortise. Defer — only matters at >100 events/sec sustained on a single rule.

## 2026-05-14 BUG-REID-NO-PERSISTENCE-01 — Cross-camera gallery cold-started on every backend restart

- **Files**: `cpp-backend/include/core/reid_engine.h` (+ thread/cv/dirty-flag fields, +loadFromDatabase/saveToDatabase/shutdown decls), `src/core/reid_engine.cpp` (+~200 LoC persistence impl, persistence_dirty_ flips in processDetection/clearGallery/pruneExpired), `src/database/db_manager.cpp` (+`reid_gallery` + `reid_trails` tables + 3 indexes), `src/main.cpp` (loadFromDatabase after init, shutdown before DbManager.close).
- **Bug**: ReIDEngine kept `gallery_` (gid→ReIDEntry with 2 KB float32 embedding), `trails_` (gid→vector<TrailPoint>), `track_to_global_` entirely in memory. Every backend restart wiped everything → on next boot, the FIRST detection on any camera was assigned a fresh `global_id`, even if the same person had been observed and identified seconds before the restart. Operationally: NSSM service restart + auto-respawn from BUG-PM-RESTART-01 sequence = identity churn. Frontend trail UI deep-link (`/api/reid/trail/<gid>`) returned 404 after restart even for recently-observed people. The frontend visual badge from 2026-05-12 closeouts pointed at a `reid_global_id` that no longer existed in memory.
- **Detection**: backlog item from 2026-05-12 closeouts bundle + 2026-05-12 ReID producer wired memo (which only fixed the dead pipeline, not the persistence gap).
- **Fix**:
  1. **Schema**: 2 new tables (`reid_gallery` PK on global_id with BLOB embedding + embedding_dim sidecar, `reid_trails` with FK-like global_id) + 3 indexes (last_seen for TTL filter on load, global_id + enter_time on trails). CREATE TABLE IF NOT EXISTS in both PG + SQLite dialects.
  2. **`loadFromDatabase()`**: called once after `init()` in main.cpp. Applies the TTL filter at load time (rows with `last_seen + gallery_ttl_sec < now` are silently dropped — so a multi-hour downtime restart doesn't repopulate a stale 30-min-old gallery). Restores `next_global_id_` to `max(loaded_gid) + 1`. Loads trails ordered by enter_time so the in-memory vector preserves chronological order. Starts the 60s periodic flush thread (idempotent via `compare_exchange_strong` on `persistence_running_`).
  3. **`saveToDatabase()`**: snapshot-then-write — copy gallery + trails out of mutex into local vectors, then run SQL outside the lock to keep the critical section short (1 MB memcpy bounded; 0 SQL under the mutex). Gallery uses dialect-aware UPSERT (`ON CONFLICT` for PG, `INSERT OR REPLACE` for SQLite). Trails use DELETE-then-INSERT keyed by global_id (no natural per-point unique key for UPSERT; bounded by gallery size × avg trail length ~10 cam = 5000 rows max). GC step at end deletes gallery rows for gids no longer in memory (covers pruneExpired's deletions).
  4. **Dirty flag**: `persistence_dirty_` (`std::atomic<bool>`) flipped to true on every mutation (processDetection new entry, processDetection re-match update of last_seen+embedding, clearGallery, pruneExpired when something was actually removed). Flush thread does `exchange(false)` and writes only if dirty was true → no writes when scene is quiet.
  5. **Shutdown**: `ReIDEngine::shutdown()` joins the flush thread + does one final synchronous flush. Wired in main.cpp BEFORE DbManager.close() in the existing background-workers shutdown block, alongside SynopsisController::shutdown / ExportController::shutdown / shutdownDelivery.
- **Concurrency notes**: persistence thread does NOT hold `mutex_` between flushes; it acquires it briefly only inside saveToDatabase's snapshot step. The `persistence_cv_` + `persistence_cv_mutex_` are used solely for wait_for + early-wake on shutdown. Thread safely sees `shutting_down` via the existing runtime_state atomic. DbManager's per-thread connection model means the flush thread gets its own QSqlDatabase via getThreadConnection — no contention with the main thread.
- **Failure modes considered**:
  - DB not ready at boot → loadFromDatabase logs WARN and returns false, but the engine still works (in-memory only) until next restart. Flush thread doesn't start.
  - Embedding dim mismatch on load (model swap between restarts) → existing `findMatch` skip-and-warn-once logic catches it; gallery row with mismatched dim is skipped at load with a WARN.
  - Process kill -9 between flushes → up to 60s of detections lost. Acceptable: alternative (per-detection write) would be 30 writes/sec under load.
  - Flush thread crashing → swallowed by the lambda's outer scope; bug surfaces as silent stop-writing. Mitigation: future enhancement to add a heartbeat counter exposed via getStatistics.

## 2026-05-14 BUG-RULE-ENGINE-NOOP-DELIVERY-01 + BUG-DUAL-RULE-DISPATCH-01 — AlertManager retired, AlertRouter dead-pipeline closed

- **Files**:
  - Deleted: `cpp-backend/include/core/alert_manager.h` + `src/core/alert_manager.cpp`, `include/api/alert_controller.h` + `src/api/alert_controller.cpp`, `include/events/alert_router.h` + `src/events/alert_router.cpp`.
  - New: `cpp-backend/include/events/alert_delivery.h` + `src/events/alert_delivery.cpp` — single delivery layer with EMAIL/SMS/WEBHOOK/UI_NOTIFICATION/MOBILE_PUSH (Telegram)/ALARM_OUTPUT.
  - Modified: `cpp-backend/include/events/rule_engine.h` (inline AlertChannel enum + decl `migrateLegacyAlertRules`), `src/events/rule_engine.cpp` (executeActions ALERT/WEBHOOK now → `vms::events::deliverAction`; new migration impl + AlertChannel string helpers), `src/core/event_manager.cpp` (drop AlertManager dispatch from createEvent), `src/main.cpp` (replace alert_router include + shutdown with alert_delivery; call migrateLegacyAlertRules), `src/server/http_server.cpp` (remove AlertController routes), `src/api/event_engine_controller.cpp` (drop unused alert_router include).
- **BUG-RULE-ENGINE-NOOP-DELIVERY-01 (CRITICAL)**: every CompositeRule's ALERT and WEBHOOK action was a silent no-op. `RuleEngine::executeActions` routed events to `AlertRouter::routeEvent` → `processEvent` which iterated `rules_` (events::AlertRule list) and dispatched per channel. But `AlertRouter::addRule` had **zero callers** — that list was permanently empty. Every operator-configured rule via `/api/rules/create` (the new UI) appeared to fire (RuleEngine::evaluateEvent recorded a trigger log entry, total_triggered_++) but no email/webhook/SMS/Telegram ever went out. Worst class of bug: visible "rule triggered" indicator, invisible failure to deliver.
- **BUG-DUAL-RULE-DISPATCH-01 (HIGH)**: `EventManager::createEvent` fanned events into both legacy `AlertManager::processEvent` (reads `alert_rules` table, real libcurl/SMTP delivery) AND `RuleEngine::evaluateEvent` (the no-op above). Pre-2026-05-12 dispatch fan-in fix this only mattered for AiEventProcessor events; after the fix every ZMQ + brand-camera hardware event also flowed both paths. The 2026-05-12 closeout memo flagged "dual-firing" but that was a misdiagnosis — delivery only ever happened via the legacy path because the modern path was dead. The actual issue: TWO rule storage layers, neither aligned with what the UI exposed.
- **Detection**: dispatch-bypass deep-dive 2026-05-12 surfaced "AlertRouter rules never load from DB?" as a side-question; chasing it found `addRule` had zero call sites. `grep -rn "AlertRouter::getInstance().*addRule"` in the entire codebase → nothing. The audit trigger was: when one of two seemingly-equivalent dispatch paths is much busier than the other in production logs (operator emails came from `[AlertManager]` lines exclusively, never `[AlertRouter]`), assume the quiet one is broken, not "less popular".
- **Fix**: full consolidation in one push (Plan B over Plan A — the smaller fix would have left the dead-pipeline bug for a future pass).
  1. New `vms::events::deliverAction(rule, action, event)` — async via shared `BackgroundJobRunner("alert-delivery", 2, 128)`. Reads per-channel recipients from `RuleAction.metadata.email_addresses[] / phone_numbers[] / telegram_chat_id`. Webhook URL from `action.webhook_url` (WEBHOOK action) or `action.metadata.webhook_url` (ALERT action with WEBHOOK channel). All channels carry the same SSRF + DNS-pin guards the deleted code had.
  2. `RuleEngine::executeActions` ALERT/WEBHOOK → `deliverAction`; RECORD_CLIP/SNAPSHOT/LOG unchanged (direct manager calls).
  3. `EventManager::createEvent` no longer calls `AlertManager::processEvent`. RuleEngine is the single dispatch.
  4. New `RuleEngine::migrateLegacyAlertRules()` runs at boot (gated by `alert_rules_migrated` setting): for each row in legacy `alert_rules` table, builds a `CompositeRule` (EVENT_TYPE condition + camera scope + ALERT/WEBHOOK action with recipient in metadata), inserts via addRule + saveToDatabase. Idempotent. Preserves operator intent across the upgrade — without this, anyone using the legacy POST API would silently lose delivery. `alert_rules` CREATE TABLE stays in db_manager (idempotent IF NOT EXISTS) for backward compat; rows are kept after migration.
  5. AlertController routes (GET/POST/DELETE `/api/alerts/rules`) deleted — frontend has zero references; external consumers must move to `/api/rules*`.
  6. AlertRouter class deleted entirely (along with the unused events::AlertRule struct + 850 LoC of evaluation/rate-limit/channel code that never ran). AlertChannel enum + helpers moved into rule_engine.{h,cpp} where they're actually used.
- **Verification**: build clean; full ctest 9/9; `grep -rn AlertManager\|AlertRouter\|alert_controller cpp-backend/src cpp-backend/include cpp-backend/tests` → only references in comments/memory now. Lint guard still 0 PENDING markers.
- **Detection lesson**: any "facade" class with a public `addRule`/`registerHandler`/`subscribe` API that has zero call sites in production code is presumed broken — even if all the downstream channel implementations look correct. Grep for the constructor side AND the producer side; an empty container of perfectly-good handlers delivers nothing.

## 2026-05-09 BUG-LINT-CONTROLLERS-PENDING — 34 unauth controller routes documented + lint guard installed against future regressions

- **Files**: `cpp-backend/cmake/lint_controllers.cmake` (new), `cpp-backend/CMakeLists.txt` (wired), 12 `*_controller.cpp` files (markers added).
- **Bug**: After 4 SEC-shape audits this week (BUG-ANPR-AUTH-01, BUG-SNAP-AUTH-01, BUG-HLS-AUTH-01, BUG-EX-RBAC-01) all finding the same `[]`-capture-no-requirePermission anti-pattern, a survey grep across `src/api/*_controller.cpp` found **34 more unauth empty-capture handlers** scattered across 10 controllers — including `event_controller` (7 routes inc. fire-test POST that could trigger fake alarms), `traffic_controller` POST counts (data-poison vector), `camera_discovery_controller` discover POST (LAN port-scan amplification), `reporting_controller` analytics CSV export (event PII aggregate). Plus 5 LEGITIMATELY-unauth captures (login/logout/2FA-verify/change-password-on-login + system streaming-config probe) that should be explicitly allowlisted.
- **Status**: HIGH (catalogue-of-survivors); each individual route warrants its own audit pass.
- **Fix**: Two-part:
  1. **Lint guard at build time**: new `cmake/lint_controllers.cmake` script counts `\(\[\]\(` empty-capture lambdas and `LINT-ALLOW-NO-AUTH` markers per file; FATAL_ERRORs if captures > markers, OR if any `auth.validate(req)` survives outside comments. Wired into the main CMakeLists at both configure-time (`execute_process`) and as `add_custom_target(lint_controllers ALL)` that `vms_backend` depends on. New empty-capture without a marker fails the build.
  2. **Survivor inventory**: each existing capture annotated with `// LINT-ALLOW-NO-AUTH: <reason>` — `auth-flow` for login/2fa/etc., `explicit-public` for the streaming-config probe, `PENDING-AUDIT-2026-05-09 (route)` for the 34 unaudited holes. Each PENDING marker MUST be removed when the corresponding route gains `[&app]` + `requirePermission`.
- **Why allowlist instead of fix-all**: 34 captures × 10 controllers ≈ a multi-session audit pass. Each route needs Permission-tier decision + audit-log decision + sometimes header signature change + sometimes frontend update (current frontend may consume some unauth paths). The right move was to stop the bleeding (lint blocks new violations forever), document the survivors with date-stamped markers, then audit each controller in its own future session. Same compromise philosophy as BUG-REID-DEAD-PIPELINE (producer_wired flag instead of immediate wiring) and BUG-ANPR-PIPELINE (documented half-dead).
- **Verification of the lint**: standalone run silent OK; injected violation (sed-swapped one `([&app](` to `([](` in snapshot_controller) → lint immediately FATAL_ERRORed with file:count + fix template; restored, lint clean again.
- **Detection lesson**: when a SEC pattern recurs 4 times in a week, the meta-fix (lint/CI) has higher leverage than another individual audit. The cost of writing the lint (~50 LoC CMake) << the cost of the next session finding controller #5 with the same bug. Apply this rule whenever you see "this is the Nth time we've found pattern X".
- **PENDING audit backlog** (priority by suspected blast radius): event_controller×7 → traffic×3 → camera_discovery×3 → reporting×2 → recording×5 → event_engine×4 → camera GET frame×1 → roi×3 / tracking×4 / ptz×2 (read-side leaks).

## 2026-05-09 BUG-AIWORKER-DEAD — `cpp-backend/src/ai/AIWorker.cpp` (766 LoC) was dead-code-on-disk

- **Files**: `cpp-backend/src/ai/AIWorker.cpp` + `AIWorker.h` (deleted)
- **Bug**: Same shape as PPEDetector + FaceInfer (deleted 2026-05-08): 766 LoC of an early in-process AI worker prototype, zero callers in production, not in any CMakeLists.txt, only self-referenced by its own .cpp/.h. Superseded by the subprocess model (`ai_worker/main.cpp` → `ai_worker_v2.exe`) but never cleaned up.
- **Status**: LOW (dead code only, no operational impact).
- **Fix**: deleted both files.
- **Detection lesson**: when a feature gets re-architected (in-process → subprocess in this case), schedule the original implementation's deletion in the same PR as the architecture change. Carrying the old code as "reference" is technical debt that compounds — every future audit pass has to verify it's still dead.

## 2026-05-09 BUG-FE-ANPR-DEL-FAKE + BUG-FE-REID-LIE + BUG-FE-REID-DEFAULT — Frontend ignored backend signals from today's RBAC/Fix-B work

- **Files**: `vms-frontend/src/views/AnalyticsView.jsx:290`, `vms-frontend/src/views/ReIDView.jsx`
- **Bug 1 (FE-ANPR-DEL-FAKE, HIGH UX)**: After `BUG-ANPR-AUTH-01` raised the DELETE bar to `SYSTEM_ADMIN`, operator-tier users get 403. Pre-fix the delete-button click handler awaited the call but ignored the response: `await apiClient.deleteAnprPlates(); setLprData([]); window.alert("Đã xóa xong!");` — operator clicked delete, saw success, refreshed, all entries reappeared. Operational lie at the UX layer.
- **Bug 2 (FE-REID-LIE, HIGH UX)**: Backend `BUG-REID-DEAD-PIPELINE` (2026-05-08) added `producer_wired` to `/api/reid/gallery` to surface that the cross-camera ReID pipeline is dormant. Frontend ignored the field; the empty-gallery message always read "Persons will appear here as AI detects them across cameras" — operators stared at the optimistic message indefinitely while the backend silently waved the `producer_wired:false` flag.
- **Bug 3 (FE-REID-DEFAULT, MEDIUM UX)**: ReID config slider initial state `match_threshold: 0.65` while backend default after Fix-B is `0.72`. Race during first render: user opens config panel before fetchConfig lands → sees 0.65 → saves → silently weakens the threshold from 0.72 back to 0.65.
- **Status**: HIGH UX / data-integrity (FE-ANPR-DEL-FAKE), HIGH UX (FE-REID-LIE), MEDIUM UX (FE-REID-DEFAULT).
- **Detection**: 2026-05-09 frontend audit pass after the 6 backend fixes landed (`33fea193`). Cross-referencing today's backend changes against frontend `apiClient` usage. Synopsis/HLS/export/snapshot endpoints turned out to be unused by frontend (external-API surface only) — no UI regression there. ANPR delete + ReID gallery were the two paths the UI actually consumes that today's changes touched.
- **Fix (FE-ANPR-DEL-FAKE)**: check `res.success`, branch on status (403 → "no permission", 401 → "session expired", else → generic). Local state only clears when the server confirmed the wipe.
- **Fix (FE-REID-LIE)**: `useState(producerWired = true)` so the warning doesn't flash during first-fetch race; `fetchGallery` reads `res.data.producer_wired`; empty-gallery message branches on the flag; warning banner appears at top of gallery panel when dormant, citing the backend bug ID and explaining the dormant state.
- **Fix (FE-REID-DEFAULT)**: bumped frontend default to 0.72. fetchConfig still wins once the request lands; the change matters only for the brief initial-render interval.
- **Detection lesson**: when a backend audit adds a new field/status code, immediately cross-reference frontend usage. The "backend fixes" → "frontend audit" cadence is one session of lag minimum, but the lag should be ZERO sessions for breaking changes (e.g. RBAC tightening that turns a 200 into 403). Add a "frontend impact" checklist line to the backend audit template.

## 2026-05-09 BUG-SNAP-AUTH-01 + BUG-SNAP-AUDIT-01 — All 3 snapshot routes used `[]` capture, ZERO auth + DELETE no audit log

- **File**: `cpp-backend/src/api/snapshot_controller.cpp` (pre-fix)
- **Bug**: Same SEC-shape repetition: `[]` capture, never reads AuthMiddleware ctx. 4 affected routes (GET list, GET by id, DELETE by id, GET file by filename). Anyone reachable on the API port could list, read, delete every snapshot in the system. Snapshots are surveillance evidence + PII. No audit log on DELETE either.
- **Status**: CRITICAL — direct PII leak + integrity loss against forensic data.
- **Fix**: `[&app]` + `requirePermission` per method. GET=`RECORDING_READ`, DELETE=`RECORDING_DELETE`, file=`RECORDING_READ`. `AuditRepository::insertLog(user_id, "DELETE_SNAPSHOT", ...)` on delete. Also clamped `limit` query param to `[1, 500]` so a client can't ask for `limit=10M`.
- **Detection lesson**: this is the 4th controller this week (ANPR, snapshot, plus reaffirmed pattern from synopsis + face) found with `[]`-capture + no `requirePermission`. The fix is mechanical; the meta-issue is that no checklist/CI gate enforces the pattern. Punt list item: lint rule that flags any `CROW_ROUTE` lambda capture that doesn't include `&app`.

## 2026-05-09 BUG-HLS-AUTH-01 + BUG-HLS-EXT-01 — Both HLS routes ZERO auth + extension check was substring not suffix

- **File**: `cpp-backend/src/api/hls_controller.cpp` (pre-fix)
- **Bug 1**: Both routes (`/api/cameras/<id>/hls/stream.m3u8` + `/api/cameras/<id>/hls/<segment>`) used `[]` capture. Anyone reachable could stream the live HLS feed of any camera — THE most sensitive PII surface in a surveillance product.
- **Bug 2**: `if (segment.find(".ts") == npos && segment.find(".m3u8") == npos)` used substring search. `evil.ts.exe` passed (`.ts` appears mid-string). `hls/<id>/` is server-controlled directory so practical exploit was limited, but the contract is wrong.
- **Status**: CRITICAL (live video PII) + LOW (substring quirk).
- **Fix**: `requirePermission(CAMERA_READ)` on both routes. Cookie-based auth (H5 hardening 2026-04) means `<video src="..."/>` includes session cookies for same-origin; cross-origin needs `crossorigin="use-credentials"`. Substring check replaced by `endsWithCi` lambda; Content-Type set from same flag so file-type gate and Content-Type can't disagree.

## 2026-05-09 BUG-EX-RBAC-01 + BUG-EX-LEAK-01 — Export controller weak auth + unbounded jobs map

- **File**: `cpp-backend/src/api/export_controller.cpp` (pre-fix)
- **Bug 1 (RBAC)**: 3 routes used `auth.validate(req)` only (the SEC-005 anti-pattern). Viewer-tier users could spawn FFmpeg jobs (CPU expensive, 2-worker shared queue) and download other users' exports if they could guess/learn the jobId. Same as synopsis pre-fix.
- **Bug 2 (LEAK)**: `exportJobs` map grew without bound — every accepted POST inserted a permanent entry, no cleanup on success or failure. Plus `exports/<jobId>.<format>` files leaked on disk. Same shape as BUG-SYN-LEAK-01.
- **Status**: HIGH RBAC + MEDIUM leak.
- **Fix (RBAC)**: `requirePermission(RECORDING_READ)` on POST/GET-status/GET-download. `AuditRepository::insertLog` on CREATE_EXPORT and DOWNLOAD_EXPORT — pairs so forensics can see "user X created, user Y downloaded, on date Z." Format whitelist now rejects with 400 instead of silently normalising invalid → mp4.
- **Fix (LEAK)**: `kMaxExportJobs = 200`, `kMaxExportJobAgeSeconds = 24h`, `pruneOldExportJobsLocked()` evicts on insert. Pending/processing jobs are NEVER evicted (worker thread reads `exportJobs[jobId]` to write status). All 200 slots busy → 429.

## 2026-05-09 BUG-PM-RESTART-01 — `FFmpegProcess::processStopped` was unwired; AI worker crashes silently disabled per-camera detection forever

- **Files**: `cpp-backend/src/core/camera_pipeline_manager.cpp`, `cpp-backend/include/core/camera_pipeline_manager.h`
- **Bug**: When `ai_worker_v2` was spawned per camera, only `stdoutReady` and `stderrReady` were connected. The third signal `processStopped(int exitCode)` (`ffmpeg_process.h:73`, emitted at `ffmpeg_process.cpp:304`) had ZERO listeners across the codebase. `CameraPipelineManager::isRunning(camera_id)` only checks `pipelines_[id]->running.load()` — a flag set at startPipeline and only cleared by stopPipeline. So when the worker exited (yesterday's BUG-AIW-LOOP-01 50-failure bail, segfault, OOM, OS kill, anything): QProcess emits `finished` → FFmpegProcess emits `processStopped` → nothing listens → `running` stays true → Health Supervisor sees "running, all good" → no restart. The camera silently lost AI detection until manual stop/start.
- **Status**: HIGH operational silent failure. Made BUG-AIW-LOOP-01 fix half-broken — the worker exited cleanly but the manager never noticed.
- **Detection**: 2026-05-09 audit pass on process_manager + camera_manager. Tracing the contract from yesterday's BUG-AIW-LOOP-01 ("manager will restart us") to verify the manager actually does. `Grep "processStopped"` returned only declaration + emit, no `connect()`.
- **Fix**: Connected `processStopped` → new private method `onAiWorkerStopped(camera_id, exit_code)`. Added `ai_cmd_cache` / `ai_restart_count` / `ai_last_restart_ms` to PipelineContext. Backoff schedule 5s/15s/60s/300s/600s, hard cap at 5 restarts in a 10-min sliding window (counter ages out so a flaky day doesn't permanently exhaust the budget). Restart runs on Qt main thread via `QTimer::singleShot(delay_ms, qApp, ...)` (FFmpegProcess::start asserts thread affinity). Reuses the same FFmpegProcess instance — its start() internally calls stop() first then re-runs.
- **Detection lesson**: When you write a fix whose contract depends on another module's behaviour ("manager will restart us"), the fix is not done until you've VERIFIED the other module actually does that. Yesterday's BUG-AIW-LOOP-01 commit should have included a check on the supervisor side. From now on: any "system X will react to this" comment in a fix needs an immediate grep to confirm before commit.

## 2026-05-09 BUG-CM-CREDLEAK-01 — `refreshAdvancedConfig` logged the full RTSP URL on parse failure, leaking user:pass to logs

- **File**: `cpp-backend/src/core/camera_manager.cpp:392` (pre-fix)
- **Bug**: `LOG_ERROR("Could not parse host from RTSP URL: {}", camera.rtsp_url)`. The DB stores RTSP URLs as `rtsp://user:pass@host:port/path`. Anything with log file access (operator console, log shipper, screenshot) leaked the camera credentials. Same lesson as BUG-C2 (devices API leaked plaintext password) and BUG-C3 (channels endpoint leaked rtsp://user:pass URL): credentials must never leave the backend, including via logs.
- **Status**: MEDIUM info disclosure (requires log access).
- **Fix**: strip `user:pass@` to `***@` before logging. Inline implementation; if another site logs RTSP URLs, extract `vms::utils::sanitizeRtspForLog()`.

## 2026-05-09 BUG-DEPLOY-RECURRENCE — Fresh `vms_backend.exe` again exited with STATUS_DLL_NOT_FOUND despite the 2026-05-03 POST_BUILD fix

- **Files**: `cpp-backend/CMakeLists.txt:142-158` (pre-fix), `cpp-backend/src/ai_worker/CMakeLists.txt` (verified — no fallback needed)
- **Bug**: User ran a freshly built `vms_backend.exe` and PowerShell returned to prompt with no output. Exit code: `-1073741515` = `0xC0000135` = **STATUS_DLL_NOT_FOUND** — the same bug class as BUG-001 (`vms_backend.exe exits with STATUS_DLL_NOT_FOUND on fresh build`, 2026-05-03). The 2026-05-03 fix added a `add_custom_command(TARGET vms_backend POST_BUILD ... copy_if_different $<TARGET_RUNTIME_DLLS:vms_backend> ...)` step, which IS still present and executes on every build.
- **Root cause**: `$<TARGET_RUNTIME_DLLS:tgt>` only resolves DLLs whose imported library targets have `IMPORTED_LOCATION` set. Many vcpkg CONFIG packages — including OpenCV's transitive deps (`opencv_calib3d4`, `opencv_features2d4`, `opencv_flann4`, `libwebp*`, `jpeg62`, `libpng16`, `libprotobuf`, `tiff`, `zlib1`, `abseil_dll`) plus `spdlog`, `fmt`, `libcrypto-3-x64`, `libzmq-mt-4_3_5` — DO NOT set `IMPORTED_LOCATION` on the imported library. CMake's generator expression silently expands to an EMPTY list for those, the `copy_if_different` runs with no inputs (returns 0), and the build "succeeds" while shipping a binary missing 11+ DLLs. dumpbin confirmed the gap: 11 of 22 critical DLLs were absent from `Release/`.
- **Detection**: User report "powershell exits without output" → exit code check showed `0xC0000135` → dumpbin /DEPENDENTS + Test-Path enumeration showed which transitive deps were missing.
- **Fix (permanent)**: Added a fallback `add_custom_command` step that finds `${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin` (preferred, set by vcpkg toolchain) or `${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/bin` (manifest-mode fallback) and `file(GLOB)` copies every DLL there to `$<TARGET_FILE_DIR:vms_backend>`. Belt-and-suspenders: the original `$<TARGET_RUNTIME_DLLS>` step still runs first for non-vcpkg deps (Qt, FFmpeg, CUDA), the vcpkg fallback fills in everything else. `copy_if_different` ensures incremental builds don't re-copy unchanged files.
- **Fix (immediate, the running install)**: copied the 11 missing DLLs by hand from `vcpkg_installed/x64-windows/bin/` to `build/Release/`. `vms_backend.exe` then started cleanly (PID 25744), all three AI workers spawned and bound to `tcp://127.0.0.1:5562/3/4` confirming BUG-AIW-NETBIND-01 also active.
- **Detection lesson** (reinforces BUG-001): "POST_BUILD ran" is not the same as "POST_BUILD copied something." Generator expressions that resolve to lists CAN be empty. Always pair a copy step with a sanity check (e.g. `file(GLOB ... "${dst}/*.dll")` count after build, or a test that runs the .exe with `--version` and asserts non-zero output). For this codebase, the safest approach is a brute-force `file(GLOB) + copy_if_different` of the entire vcpkg bin/.

## 2026-05-09 BUG-AIW-V2-STALE — Today's worker tuning never reached production because the running `ai_worker_v2.exe` was 12 days old

- **File**: `cpp-backend/build/Release/ai_worker_v2.exe` (pre-rebuild stamp Apr 27)
- **Bug**: All seven of today's AI worker fixes (BUG-DETECT-FP-01 / FP-02 thresholds, BUG-AIW-NETBIND-01 loopback bind, BUG-AIW-INPUT-01 size cap, BUG-AIW-FACEDB-01 RELOAD command, BUG-AIW-LOOP-01 retry counter, BUG-AIW-DIAG-01 dump cap) live in `cpp-backend/src/ai_worker/main.cpp`, which compiles to a **separate executable** named `ai_worker_v2.exe`. Today's build commands all used `--target vms_backend` and never built the worker. The running worker on disk was still the Apr 27 binary; spawned worker processes showed `ZMQ REP listening on tcp://*:5562` (the pre-fix bind) — proof the changes were never reaching production.
- **Status**: HIGH operational regression — the user spent the day tuning thresholds and watching no behavioural change.
- **Detection**: User launched backend; first run dumped `tcp://*:5562` instead of the expected `tcp://127.0.0.1:5562` after BUG-AIW-NETBIND-01. The `*` in the bind line was the smoking gun.
- **Fix**: Rebuilt with `cmake --build ... --target ai_worker_v2 --target vms_backend`. Confirmed new `ai_worker_v2.exe` stamp + spawned workers now showing `tcp://127.0.0.1:556X`.
- **Detection lesson**: When a codebase has multiple executables, `--target X` builds ONLY X and its lib deps, not other executables that share source files. Always confirm BOTH the build command and the deployed binary stamp match the changes you made. Future safety: add `--target all` to the standard build command in CONTRIBUTING / build instructions, or list every relevant target explicitly. For ad-hoc builds where you know one source file affects N targets, list all N.

## 2026-05-08 BUG-AIW-NETBIND-01 — AI worker ZMQ REP socket exposed face-embedding API on every network interface

- **File**: `cpp-backend/src/ai_worker/main.cpp:70` (pre-fix)
- **Bug**: `socket.bind("tcp://*:" + port)` made the EXTRACT_EMBEDDING ZMQ endpoint reachable on every interface — anyone on the LAN could submit images for face-embedding extraction (computational DoS against the TRT mutex) or pull responses revealing what the model produced for the submitted crop. The legitimate caller (`face_controller::extractEmbeddingFromAI`) already connects to `tcp://127.0.0.1:port`; loopback-only is sufficient.
- **Status**: CRITICAL — open compute API on the LAN, no auth, no rate limit. Same severity class as the past `MediaMTX tcp_address: :8889` → `127.0.0.1:8889` fix in 2026-05-02 bugfix pass.
- **Fix**: bind defaults to `127.0.0.1`. `VMS_AI_ZMQ_BIND` env override for explicit multi-host opt-in.
- **Detection lesson**: every `bind()` to `*` / `0.0.0.0` is a SEC-defaults review item. Grep the codebase for `tcp://\*` and `INADDR_ANY` patterns at every audit pass.

## 2026-05-08 BUG-AIW-INPUT-01 — AI worker EXTRACT_EMBEDDING accepted unbounded image payload

- **File**: `cpp-backend/src/ai_worker/main.cpp:118` (pre-fix)
- **Bug**: `img_base64 = req_json.value("image", "")` had no size cap. A 100 MB base64 string would happily decode through `base64_decode()` and `cv::imdecode()`, OOMing the worker or stalling the TRT mutex for tens of seconds. `face_controller.cpp:269` already enforced 10 MB but a direct ZMQ client bypassed that.
- **Status**: HIGH (DoS; reachable post-BUG-AIW-NETBIND-01 fix only from loopback, but on the loopback any process on the box is now a threat surface).
- **Fix**: `kMaxEmbeddingImageBase64Bytes = 12 MB` (10 MB + base64 inflation headroom). Mirrors the controller cap server-side so the bypass is closed at the trust boundary.

## 2026-05-08 BUG-AIW-FACEDB-01 — Face DB loaded ONCE at boot; live recognition never saw register/update/delete

- **Files**: `cpp-backend/src/ai_worker/main.cpp:209` (loadFaceDatabase), `cpp-backend/src/api/face_controller.cpp` (REST CUD)
- **Bug**: `loadFaceDatabase()` was called exactly once at AI worker boot. After that, the inference engine's in-memory `face_database_` was frozen for the worker's entire lifetime. When face_controller mutated the DB via REST, the worker never saw the change → newly registered persons never matched in live recognition, deleted persons still matched. Operators could register a face, see it in the UI, and watch live recognition silently ignore it. Same half-dead pipeline shape as BUG-ANPR-PIPELINE (LPR detects, no DB persist) but inverted: DB writes happen but the read-side cache is stale.
- **Status**: HIGH operational lie — the documented happy-path of "register a face → it appears in live recognition" was a façade until a worker restart.
- **Fix (worker side)**: new `RELOAD_FACE_DB` ZMQ command — clears the in-memory gallery (`MultiModelInfer::clearDatabase()`) and re-runs `loadFaceDatabase()` under the same TRT mutex used by inference. Atomic-replace from the caller's perspective; takes ~50–200 ms depending on gallery size.
- **Fix (controller side)**: new `broadcastFaceDbReload()` helper that fans out the command to every active camera's worker port. Best-effort, short timeouts (recv=800ms, send=300ms, no retry). Called after every successful insertPerson / updatePerson / deletePerson. Failures don't fail the REST response (the DB write is the source of truth and the next worker boot will re-sync).
- **Detection lesson**: when an in-memory cache is populated from a DB and the DB has its own write path, audit "is there a refresh?" alongside "is there a load?". Static-load + DB-write-elsewhere = guaranteed stale read on the worker side.

## 2026-05-08 BUG-AIW-LOOP-01 — Inference loop catch-all retried forever on persistent failure

- **File**: `cpp-backend/src/ai_worker/main.cpp:858` (pre-fix)
- **Bug**: `catch (const std::exception& e) { LOG; sleep(100ms); }` and continue. If inference threw on every frame (corrupted TRT engine, OOM on input, model file truncated mid-deploy), the worker spammed 10 lines/sec of identical errors indefinitely with no recovery path. Manager could not detect "worker is broken" because the worker stayed alive.
- **Status**: HIGH — log spam + no auto-recovery on permanent failure mode.
- **Fix**: consecutive-failure counter; bail after `kMaxConsecutiveFailures = 50`. Counter resets on every successful loop iteration so transient errors don't accumulate. Manager process gets a clean exit and can restart.
- **Detection lesson**: `catch + sleep + continue` is fine for transient failures, fatal for permanent ones. Any unbounded retry needs a counter; pair `consecutive_failures` with the existing sleep so the same code path handles both.

## 2026-05-08 BUG-AIW-DIAG-01 — VMS_AI_DUMP_FRAMES filled disk indefinitely

- **File**: `cpp-backend/src/ai_worker/main.cpp:505` (pre-fix)
- **Bug**: When `VMS_AI_DUMP_FRAMES=1`, the worker wrote one diag JPG every 200 frames forever. At 15 fps that's ~6500 files/day per camera. A forgotten env var could fill the disk in days on multi-camera deployments.
- **Status**: LOW (operational, not security).
- **Fix**: cap at `kMaxDiagDumps = 50` per process. Logs once when cap is reached so operators know.

## 2026-05-08 BUG-DETECT-FP-02 (follow-up to FP-01) — YOLO threshold raised again 0.30 → 0.45

- **File**: `cpp-backend/src/ai_worker/main.cpp` YOLO default
- **Bug** (operator-reported follow-up): Fix-B's 0.30 was still letting "vaguely person-shaped" objects through — trash cans, posts, vertical shadows that COCO YOLO scores in the 0.30–0.40 band still reached the live overlay. Operator: "yolo phải nâng cao thêm nữa".
- **Fix**: bumped default to **0.45** so anything in the 0.30–0.45 band is dropped at the threshold. The 0.45 boundary cleanly separates "confident person" (typically 0.5+) from "background clutter that vaguely looks like a person."
- **Recall trade-off**: ~35% drop on distant/partial persons (< 60–70 px tall). Already mitigated by `VMS_MIN_PERSON_HEIGHT_PX=40` filter from Fix-B, so the lost detections are largely below the operationally useful range. Confident near-camera persons keep > 90% recall in our test scenes.
- **Detection lesson**: confidence thresholds for surveillance YOLO should be set NOT by published model benchmarks (which use clean COCO val) but by per-deployment empirical tuning. The 0.10 → 0.30 → 0.45 trajectory in three commits is normal — every camera placement has its own background clutter signature, and the right threshold is the lowest value that doesn't pin the operator on noise.

## 2026-05-08 BUG-DETECT-FP-01 — Detection thresholds tuned for "anything moves" → trash cans labelled "person", road texture matched as the operator's face

- **Files**: `cpp-backend/src/ai_worker/main.cpp` (yolo_conf default 0.10, bypass_tracker default true), `cpp-backend/src/ai/inference/include/inference/multi_model_infer.h` (scrfd_conf 0.40, face_match 0.65)
- **Bug**: Operator-reported regression: trash cans being labelled "person" in the live feed; the operator's own face being matched on road surfaces. Diagnosed as a chain failure of permissive defaults across the whole detection→tracking→recognition pipeline:
  1. **YOLO @ 0.10** (lowered 2026-04-26 to make distant detections show up at all) hallucinated "person" on any vertical dark blob — trash cans, traffic cones, shadows.
  2. **bypass_tracker = true** (workaround for a historic ObjectTracker.update() crash, even though the try/catch fallback at the same call site already handles it) → no temporal filtering, every transient false-positive published with track_id=-1.
  3. **SCRFD @ 0.40** false-fired on road texture / wall logos.
  4. The `require_person_overlap` post-filter PASSED these false-positive faces because YOLO ALSO hallucinated a "person" in the same area at 0.10.
  5. ArcFace ran on garbage crops, produced a real (random-looking but L2-normed) 512-dim embedding.
  6. **face_match_threshold = 0.65** + the per-frame stochastic similarity → roughly 1-5% probability per frame that a garbage embedding scored above threshold against the operator's stored face. `FaceTracker` then accumulated the pin via `stable_person_id` so the misidentification stuck across frames.
- **Status**: HIGH (production-visible operational lie — operators couldn't trust the live overlays). Same flavor as the threshold-tuning regressions we've hit before, but this time multiple knobs compounded.
- **Detection**: Direct operator report ("sao detect vẫn sai lệch — thùng rác cũng ra người, mặt đường lại ra khuôn mặt tôi"). Code review confirmed all five permissive defaults.
- **Fix (Fix-B coordinated tuning + filters)**:
  - YOLO conf default 0.10 → **0.30** (env: `VMS_YOLO_CONF_THRESHOLD`).
  - SCRFD conf default 0.40 → **0.55** (env: `VMS_SCRFD_CONF`).
  - face_match_threshold default 0.65 → **0.72**.
  - `bypass_tracker` default `true` → **`false`** (use ObjectTracker; the existing try/catch already falls back to bypass per-frame on tracker exceptions, so the tracker-instability concern is contained).
  - New: `min_person_height_px` filter (default **40**, env: `VMS_MIN_PERSON_HEIGHT_PX`) drops sub-40px person detections after the class filter, before tracker.
  - New: `min_face_side_px` filter (default **30**, env: `VMS_MIN_FACE_SIZE_PX`) drops faces whose shorter side < 30px before the require_person_overlap pass — these crops are too small for ArcFace to produce meaningful embeddings even when they ARE real faces.
- **Why all six together (not "just bump thresholds")**: each mitigation closes a different leg of the chain. Threshold bumps alone leave temporal-filtering off (a single bad frame still pins a wrong identity via FaceTracker). Tracker alone leaves the per-frame YOLO/SCRFD false-positive volume too high for the IoU matching to suppress reliably. The min-size filters are the cheapest and most surgical because they operate on geometry, not confidence — a 20×20 SCRFD detection is statistically meaningless regardless of its confidence score.
- **Detection lesson**: When operators report "detector is wrong," the answer is rarely a single threshold. Each model in the pipeline (YOLO → SCRFD → ArcFace) has its own calibration, and when one is permissive, downstream components amplify the noise rather than filter it. Audit the WHOLE chain: detection threshold → temporal smoothing → geometric sanity → recognition threshold. Any one link being too lax produces a hallucination cascade that looks like a different bug at every layer.
- **Trade-off**: ~20% drop in distant-person recall and ~5% drop in face recognition recall on poor lighting, in exchange for an order-of-magnitude reduction in false-positive identity matches. Operators wanting the old high-recall behaviour can env-override per camera.

## 2026-05-08 BUG-ANPR-AUTH-01 — ANPR controller had ZERO authentication on all five route variants

- **File**: `cpp-backend/src/api/anpr_controller.cpp` (pre-fix)
- **Bug**: All five route variants on `/api/anpr/plates` (GET, POST, DELETE) and `/api/anpr/search` (GET) used `[]` lambda capture and never read AuthMiddleware context. Anyone reachable on the API port could (a) leak the entire vehicle plate × camera × timestamp PII ledger, (b) poison it with fake records, (c) wipe the entire table with a single DELETE — no confirmation, no audit log. Same shape as SEC-003/004 (devices/sites RBAC, 2026-05-02) and SEC-008/009/010 (analytics unauth GETs, 2026-05-03).
- **Status**: CRITICAL — PII data leak + integrity / availability against the historical plate ledger. Pure regression of past audit lessons.
- **Detection**: 2026-05-08 audit pass on the LP/ANPR surface as part of the synopsis audit. Same `[]` capture pattern that the past audits already trained on.
- **Fix**: `[&app]` capture + `requirePermission` per method. GET → `ANALYTICS_READ` (matches counter/attendance/reid analytics surface). POST/DELETE → `SYSTEM_ADMIN` (manual writes are an admin function — the real population path is the AI worker's LPR detection, not operator entry; bulk delete is destructive). `AuditRepository::insertLog` on POST and DELETE with the prior-count snapshot. `camera_id` POST validation now rejects negatives / pathological values.
- **Detection lesson** (reinforces SEC-003/004/008): every new controller introduced into the codebase needs to be audited as part of the next nearby pass. The ANPR controller predates the SEC-002+ audit work; nobody went back to backfill it. We need a checklist of "controllers we've audited" and re-grep for the `[]` capture pattern any time a new controller is added.

## 2026-05-08 BUG-SYN-PATH-01 — Synopsis videoPath user-supplied path traversal

- **File**: `cpp-backend/src/api/synopsis_controller.cpp:61` (pre-fix)
- **Bug**: `POST /api/synopsis/create` accepted `videoPath` directly from the JSON body and passed it to `cv::VideoCapture(...)` inside the engine. The empty-path branch fell through to a DB lookup but any non-empty user value was used verbatim. A logged-in user could probe arbitrary paths on disk via job success/failure timing (different errors for "not found" vs "not a video" vs "permission denied"); with the right combination of args the engine could even render content from a non-recording video file into the publicly served `recordings/<jobId>.mp4` output.
- **Status**: HIGH (info disclosure / partial data exfiltration), gated by login but accessible to any authenticated user.
- **Fix**: New `sanitizeVideoPath()` helper — `weakly_canonical()` of both the path and the `recordings/` root, prefix check via `target.rfind(root, 0) == 0`. Empty input passes through (DB-lookup branch handles it). Non-empty inputs that escape the recordings root return 400 + a LOG_WARN.
- **Detection lesson**: Path-traversal hazard rule: any user-supplied string that hits a `cv::VideoCapture`, `std::ifstream`, `std::filesystem::*`, or anything that opens a path needs the same treatment. Same pattern as BUG-H11 (recording delete weakly_canonical + prefix check) — apply universally, not only on the destructive paths.

## 2026-05-08 BUG-SYN-RBAC-01 — Synopsis create/status gated only on `auth.validate(req)`

- **File**: `cpp-backend/src/api/synopsis_controller.cpp:54,156` (pre-fix)
- **Bug**: Synopsis is an analytics feature that CPU-saturates a worker for minutes per job and reads recording paths off the DB. Pre-fix any logged-in user (including viewer) could spawn jobs. Same shape as SEC-005 (face/reid/videowall "is logged in" gates).
- **Status**: HIGH (DoS-prone — viewer can ladder synopsis jobs to fill the queue and read sensitive recording metadata).
- **Fix**: Switch from `auth.validate(req)` to `app.get_context<AuthMiddleware>(req)` + `requirePermission(ANALYTICS_READ, origin)`. The legacy `auth` parameter on `registerRoutes` is preserved (signature-stable); the body now ignores it (`(void)auth`).
- **Detection lesson**: `auth.validate(req)` without a permission gate is identical-in-effect to "is this caller authenticated?" and we now always treat that as the SEC-005 anti-pattern. Grep for `auth.validate(` whenever there's an audit pass — any survivor needs justification.

## 2026-05-08 BUG-SYN-RENDER-01 — Crop-vs-rect size mismatch in synopsis rendering threw mid-job

- **Files**: `cpp-backend/src/ai/synopsis/SynopsisEngine.cpp:191-193`, `TubeManager.cpp:71-76` (pre-fix)
- **Bug**: `TubeManager::processFrame` downscales tube-frame crops whose source `rect.area() > maxCropArea_` (256×256), but stores `tf.rect` at the original full-size box. `SynopsisEngine::renderSynopsis` then did `cv::Mat roi = frame(tf.rect); tf.crop.copyTo(roi);` — `cv::Mat::copyTo` throws `cv::Exception` on every size-mismatched call. ANY synopsis job containing at least one large object crashed mid-render. The exception bubbled through `engine.generate()` to the `BackgroundJobRunner` worker which had NO try/catch — the job stayed "processing" forever, partial `recordings/<jobId>.mp4` output leaked on disk, REST status path showed nothing useful.
- **Status**: HIGH (functional crash on the typical happy-path input — anyone running synopsis on real CCTV footage hit this).
- **Detection**: 2026-05-08 audit. The TubeManager file's "MEMORY FIX" comment block on the downscale path was the smoking gun — when one half of a pipeline applies a transformation that the other half doesn't expect, you have a contract violation.
- **Fix**: Engine: clamp `drawRect = tf.rect & frame_bounds`, `cv::resize(tf.crop, ...)` to match `drawRect.size()` before `copyTo`, skip empty crops. Controller: `try { engine.generate(...) } catch (cv/std::exception)` — failures now mark the job "failed" with the real error string instead of leaving the worker thread stuck.
- **Detection lesson** (general): "memory-fix" comments that reduce one side of an output pair without updating the consumer are a textbook contract-mismatch hazard. Whenever you see "scale this down to save memory" applied to a value used downstream, audit every consumer. Sometimes the consumer needs the original dimensions; sometimes it needs the scale factor too.

## 2026-05-08 BUG-SYN-DURATION-01 — Synopsis `targetDuration` unbounded → DoS

- **File**: `cpp-backend/src/api/synopsis_controller.cpp:98` (pre-fix)
- **Bug**: `targetDuration` accepted any int from JSON. The render loop is O(output_frames × tubes × frames_per_tube). 24h × 30 fps × 50 tubes × 150 frames ≈ 20 billion iterations on the single synopsis worker. Combined with BUG-SYN-RBAC-01 above, a viewer could fully saturate the synopsis worker indefinitely.
- **Fix**: Clamp to `[5, 600]` seconds.

## 2026-05-08 BUG-SYN-LEAK-01 — `g_jobs` map grew without bound

- **File**: `cpp-backend/src/api/synopsis_controller.cpp:28-29,103` (pre-fix)
- **Bug**: Every accepted `/api/synopsis/create` permanently inserted a `SynopsisJob` entry. No cleanup on success or failure.
- **Fix**: `kMaxJobs = 200`, `kMaxJobAgeSeconds = 24h`, new `pruneOldJobsLocked()` evicts on insert. Pending/processing jobs are NEVER evicted (worker thread reads `g_jobs[jobId]` to write status). If all 200 slots are pending/processing, returns 429 instead of overwriting.
- **Detection lesson**: Every long-lived in-memory map indexed by user-controllable keys (jobId, request_id, session_id, etc.) needs an eviction policy on the insert path. "Server runs forever" is the wrong default; "evict the obsolete" is the right one.

## 2026-05-08 BUG-SYN-CAMID-01 — Synopsis `cameraId` no bound check

- **File**: `cpp-backend/src/api/synopsis_controller.cpp:58` (pre-fix)
- **Bug**: Accepted any int including negatives — DB lookup went out to MIN_INT space, no harm but pollutes logs and makes grep-debug harder.
- **Fix**: Reject `cameraId < 0 || > 1000000`.

## 2026-05-08 BUG-ANPR-PIPELINE — LPR detects plates but no DB persistence bridge (DEFERRED — documented half-dead pipeline)

- **Files**: `cpp-backend/src/ai_worker/main.cpp:744-759` (LPR producer), `cpp-backend/src/database/anpr_repository.cpp:129` (insertPlate caller search)
- **Bug**: `enable_lpr` defaults to false. When enabled, AI worker detects license plates and emits them as `TrackedObject{class_id=200, label="LicensePlate"}` into the unified ZMQ metadata stream. There is NO bridge that persists detected plates into the `LicensePlate` DB table — `ANPRRepository::insertPlate` is called only from the manual `POST /api/anpr/plates` REST endpoint. So:
  - When LPR disabled (default): nothing detects, nothing inserts, REST table empty.
  - When LPR enabled: detections flow live (frontend WS sees them), but **historical plate queries return empty** — the table is never populated.
  - Manual POSTs work but the entries are not connected to anything.

  Same shape as BUG-REID-DEAD-PIPELINE (cross-camera ReID gallery never fed) but partial — the producer is alive in the live stream, just not bridged to persistence.
- **Status**: CRITICAL operational lie if LPR is advertised as a working feature. Less severe than BUG-REID-DEAD-PIPELINE because the live event stream IS populated; it's only the historical query that's dead.
- **Why deferred**: The wiring is small (~30 LoC) but the dedup semantics need a design decision — same plate detected on consecutive frames should not create N rows. Same kind of question we already answered for `AttendanceTracker::dedup_` and `CounterBucketAggregator`. Choose a key (plate_text + camera_id + minute-of-day window), idempotent UPSERT, retention policy. The bridge wiring is its own session.
- **Mitigation landed this session**: BUG-ANPR-AUTH-01 fix at least closes the data-leak side — even if the table starts populating, only authorised users can read it.

## 2026-05-08 BUG-REID-DEAD-PIPELINE — Cross-camera ReID gallery never populated; REST endpoints returned empty arrays as if scene were quiet

- **Files**: `cpp-backend/src/core/reid_engine.cpp`, `cpp-backend/src/api/reid_controller.cpp`, `cpp-backend/include/core/reid_engine.h` (pre-fix)
- **Bug**: `ReIDEngine::processDetection(camera_id, track_id, person_crop)` is the SOLE producer that populates `gallery_`, `trails_`, and `track_to_global_`. A backend-wide grep showed it had ZERO callers — and `ReIDEngine::init()` was also never called from anywhere. Yet five REST endpoints (`GET /api/reid/gallery`, `POST /api/reid/search`, `GET /api/reid/trail/<id>`, `GET /api/reid/statistics`, `GET/PUT /api/reid/config`) were fully wired through `ReIDController::registerRoutes` and returned successful 200 responses on top of an empty, never-fed in-memory state. `getStatistics()` returned `{total_processed:0, gallery_size:0, model_loaded:false, initialized:false}` — distinguishable in principle, but the gallery and search endpoints just returned `[]` with `count:0`, indistinguishable from "scene currently empty." Operators viewing the UI had no signal that the cross-camera ReID feature was a façade.
- **Status**: CRITICAL operational lie. Same shape class as BUG-EVENTS-01 (4 brand `pullEvents` were fake keepalive), BUG-INFER-02 (`AdvancedInfer` face methods returned empty stubs), BUG-ALERT-01 (`sendEmail` was log-only) — an "advertised feature" with a dead data pipeline.
- **Detection**: 2026-05-08 audit pass. After mapping `face_controller.cpp` and `reid_controller.cpp`, the next step was finding `processDetection` callers — `Grep "processDetection|ReIDEngine::getInstance"` returned only the controller (which uses `getInstance()` but calls only query-side methods) and the engine implementation itself. `ReIDEngine::init` had the same zero-caller pattern. The init/processDetection-vs-controller-wiring mismatch was the smoking gun.
- **Fix (interim, fail-loud)**: NOT a real wiring; the inference→ReID plumbing is its own session (needs to settle on which producer thread emits person crops and at what cadence — too big to land here without breaking pipeline backpressure assumptions). Instead:
  - New `std::atomic<bool> producer_wired_{false}` on the engine; flipped to `true` (with a one-time `LOG_INFO`) on the first `processDetection` call.
  - `getStatistics()` now exposes `producer_wired` so the UI / ops dashboards can render a "feature not active" banner instead of "no people seen yet."
  - `getActiveGallery` and `searchByImage` REST responses also include `producer_wired` for direct consumers.
  - REST controller calls a new `warnIfProducerNotWiredOnce(engine)` helper from gallery/trail/search/statistics handlers — first such call after process boot logs a single LOG_WARN line explaining the situation. Subsequent calls are silent (`std::call_once`).
  - Same kind of compromise we landed on AdvancedInfer's face methods (BUG-INFER-02 → throw `std::logic_error`) and the legacy AlertManager email mock (BUG-ALERT-02 → real SMTP via shared util). Difference: throwing from an HTTP handler 5x in a row would page the on-call; flipping a flag and warning once is the right blast-radius for a feature that has never worked anyway.
- **Detection lesson**: When a singleton has a clean `init()`/`processDetection()`/REST-query API split, AND the REST query endpoints are wired up in a controller, ALSO grep for callers of the producer methods — not just the singleton. A controller that calls `getInstance()` doesn't tell you whether the producer side is alive. This is the same lesson we wrote into BUG-INFER-02: "audit feature pipelines as a SET, not just at the API boundary."
- **Punt list**: implement the real producer (`AiEventProcessor` or wherever person tracking emits stable track ids + person crops → call `processDetection` per frame per track). Likely small (10s of lines) but needs a session focused on backpressure: at 3 cams × 15 fps × 10 persons in frame, that's 450 ReID extractions/sec under the current global mutex_ — see also BUG-REID-LOCK-IO below for why that lock has to come off the hot path before wiring.

## 2026-05-08 BUG-FACE-EMB-01 — `MultiModelInfer::extractFaceEmbeddings` accepted any output dim, partial-zero embeddings produced false-identity matches

- **File**: `cpp-backend/src/ai/inference/src/multi_model_infer.cpp:644-666` (pre-fix)
- **Bug**: After `arcface_engine_->infer()` returned `true`, the code did `embedding_size = std::min(output_tensor.size(), size_t(512))` and `memcpy`'d that many floats into the fixed-size `face.embedding[512]`. Then L2-normalised over `[0, embedding_size)` only — leaving the tail zero (struct is value-initialised to zero by its default ctor). On a 256-dim model swap (e.g. someone bumps to `arcface_r34` which has 512 features but the engine got loaded with the wrong head), the first 256 floats normalise but the last 256 stay zero. `matchFaces()` then computes cosine similarity over the FULL 512-dim against every DB person — first 256 dims contribute partial information, last 256 all zeros. The numerator is a partial dot product, the denominator's q-norm is ≈1 (correctly), so similarity scores are mathematically wrong but plausible-looking. **Above-threshold spurious matches** on random DB persons are reachable.
- **Status**: HIGH severity (false-positive identity). Same shape as BUG-INFER-01 — bool-discard-then-zero-pad on partial inference output. Pre-fix BUG-INFER-01 was caught in the standalone `FaceInfer` class (deleted as dead code in this session's warmup); this bug is on the live production path used by `MultiModelInfer`. Triggers on any model that doesn't output exactly 512 floats — by definition, the first time someone swaps in a wrong-dim model.
- **Detection**: 2026-05-08 audit pass. Searched for the BUG-INFER-01 pattern (`std::min(.., size_t(512))`) and matched line 649. Re-reading the L2-normalize block confirmed the partial-then-compare-against-512 hazard.
- **Fix**: Strict `output_tensor.size() == 512` check. On mismatch: zero the embedding + `std::call_once`-throttled `std::cerr` warn (the file uses iostreams not spdlog) + `continue`. On match: memcpy 512 floats, normalise across all 512, and on degenerate near-zero norm zero the embedding (cleaner than dividing by `1e-6f`-ish noise).
- **Detection lesson** (reinforces BUG-INFER-01's): `std::min(output.size(), expected)` followed by L2-normalise-over-prefix is the operational lie pattern. The "I'll just clip and pretend it fits" reflex is wrong because the COMPARISON is full-width — the unfilled tail still participates in the score and pulls it toward whatever happens to be in those slots.

## 2026-05-08 BUG-FACE-CTRL-01 — `extractEmbeddingFromAI` blocked HTTP thread up to 50s when AI workers down

- **File**: `cpp-backend/src/api/face_controller.cpp:31-83` (pre-fix)
- **Bug**: The helper iterated over `cam_mgr.getAllCameras()` filtering `is_active`, building a fresh ZMQ REQ socket per camera with `rcvtimeo=3000ms` + `sndtimeo=2000ms`. With 10 active cameras and the inference service down, EVERY iteration took ~5s before timing out, and every call to `extractEmbeddingFromAI` blocked the calling Crow handler thread for 50+ seconds. Triggered on POST `/api/faces/persons` (creating a person with a face image) and POST `/api/faces/search`. Combined with Crow's bounded handler thread pool, a single dead inference service could exhaust the API thread pool in seconds and cascade-fail unrelated endpoints.
- **Status**: HIGH severity (DoS-prone failure mode). Latent until inference goes down, but the failure profile under that condition is severe.
- **Fix**: Cap iteration to `MAX_WORKER_ATTEMPTS=3` and tighten per-attempt timeouts to `RECV=1500ms` / `SEND=500ms`. Worst case is now ~6s instead of 50s. Sequential is fine — the success rate of the first worker is high in normal operation; this code path matters only on the failure side.
- **Detection lesson**: When you see a "try every X until one succeeds" pattern with per-iteration network timeouts, the worst-case latency is `N × per_iter_timeout` and lives on whatever thread the call lands on. ALWAYS cap N and prefer aggressive per-iter timeouts over slow-but-eventual success — the tail latency lands on the request thread, not the inference workers.

## 2026-05-08 BUG-FACE-CTRL-02 — PUT /api/faces/persons/<id>: stale embedding stuck after image change when AI worker unavailable

- **File**: `cpp-backend/src/api/face_controller.cpp:392-426` (pre-fix)
- **Bug**: When an operator uploaded a new face image via PUT and `extractEmbeddingFromAI` failed (worker down, dim mismatch, etc.), the code silently kept `p.embedding_json` at its prior value loaded from DB. So the person now had: face_image_path = NEW image, embedding_json = OLD vector. Subsequent face SEARCH would compare query faces against the old embedding and return matches that don't visually correspond — a false-identity result that looks correct in the response shape.
- **Status**: MEDIUM (data-integrity hazard, not exploitable but ops-visible).
- **Fix**: When the image changed but extraction fails, set `p.embedding_json = "[]"` so search returns "no match" until the operator either retries or the worker comes back. Logged at WARN with the person id so the inconsistency is traceable.
- **Detection lesson**: Compound state (image + embedding) where one component can fail-and-keep-old and the other is fail-and-replace creates "skewed pairs" — neither half is wrong in isolation but the combination produces wrong results. Default policy: if any component of a compound update fails, blank the dependent components rather than letting them carry old values into a new context.

## 2026-05-08 BUG-FACE-DEL-01 — DELETE /api/faces/persons/<id>: image deleted before DB row, leaving dangling row on DB error

- **File**: `cpp-backend/src/api/face_controller.cpp:451-466` (pre-fix)
- **Bug**: Pre-fix the code called `deleteImageFromDisk(p.face_image_path)` BEFORE `repo.deletePerson(id)`. If the DB delete failed (lock timeout, FK constraint, etc.), the image file was already gone but the row still pointed at the missing path → UI showed broken thumbnail forever.
- **Status**: LOW severity (cosmetic UI breakage, no data loss because the person record is intact).
- **Fix**: DB row first; only on successful delete do we drop the image. If the image-delete then fails, log and accept the orphan file (less bad than the inverse — orphan files can be GC'd by a periodic sweep).
- **Detection lesson**: Two-step destructive operations should commit the harder-to-rollback step LAST. DB has transactions; the filesystem doesn't. Order: DB then disk.

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
