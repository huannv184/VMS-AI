# Architectural Decisions — AI Camera System

## 2026-05-15 WS subscribe RBAC — per-socket cache populated at AUTH; admin + camera-0 bypass

### Decision: Cache the allowed-camera set on the socket at AUTH time, not on every subscribe
- **Choice**: After successful WS AUTH (ticket validated, JTI not replayed), for non-admin users, call `CameraRepository::getFilteredCameras(user.id)` once and cache the resulting camera IDs in a file-scope `std::unordered_map<QWebSocket*, std::unordered_set<int>> g_allowed_cameras` guarded by a mutex. Subscribe-path lookup is `O(1)` against this map; no DB hit per subscribe.
- **Rationale**: AUTH is once-per-WS-connection (and the connection lives minutes to hours), but a single client may subscribe + unsubscribe many times during a session (camera-switcher UI, videowall mosaic). Doing the DB query at subscribe time would multiply DB load by the subscribe rate; caching at AUTH time amortises it across the whole connection. The cache is per-socket so different concurrent users with different scopes don't share state.
- **Trade-off**: cache is stale if admin updates the user's `allowed_cameras` while the WS is open. The user keeps their pre-update scope until they reconnect (logout/login, or browser tab close + reopen). Acceptable for a Phase 1 RBAC pass — the alternative (cache invalidation pub-sub from PermissionRepository to live sockets) is a much bigger surface change. Logged as a follow-up: "permission change should bump a notify signal that sockets re-load their cache".
- **Alternative considered**: lookup on every subscribe with a hot-path DB query. Rejected — even at 1 query per subscribe per session, the cumulative DB load adds noise to the batch writer's working set, and `getFilteredCameras` already does 1 SELECT cameras + 1 SELECT permissions internally.
- **Alternative considered**: encode the allowed_cameras list directly in the WS ticket JWT claim. Rejected because (a) the ticket is 30s TTL and operator might revoke camera access in that window, (b) the list could be 50+ cameras = large JWT, (c) the JWT signing key would need to be rotatable independent of the camera list.

### Decision: Admin (role_id == 1) and camera_id == 0 bypass the cache
- **Choice**: admins skip the cache entirely via a `vms_is_admin` Qt property set at AUTH success. Camera 0 (global broadcast channel) is allowed for any authed user.
- **Rationale**: admins by definition have all-cameras access (consistent with `CameraRepository::getFilteredCameras` line 252-255 admin-bypass). Camera 0 is the global event channel — config changes, audit log notifications, system alerts — that's not camera-specific data; viewers legitimately need it.
- **Trade-off (documented for follow-up)**: the per-camera EventManager dispatch ALSO broadcasts to camera 0. So a viewer subscribed to camera 0 still receives every event from every camera. The fix is broadcast-side filtering: when fanning a non-zero camera_id event out to camera 0 subscribers, check each recipient's allowed-set. Not in this pass because it touches the broadcast loop's complexity; logged as `BUG-WS-GLOBAL-FANOUT-01`.
- **Why not deny camera 0 for non-admins**: would break the "viewer subscribes to camera 0 for system notifications" pattern the frontend relies on. Better to fix the leak at broadcast time than to deny the legitimate use case.

### Decision: Side-map keyed by raw `QWebSocket*` instead of Qt property
- **Choice**: `std::unordered_map<QWebSocket*, std::unordered_set<int>>` at file-scope (anonymous namespace), guarded by `std::mutex`.
- **Rationale**: Qt's property system stores `QVariant`, which would require registering `std::unordered_set<int>` as a meta-type or wrapping in `QSet<int>` — extra ceremony for no benefit. A side-map is explicit, easy to reason about, and the mutex makes the threading model obvious (Qt main + socket-disconnect slot can both touch it).
- **Trade-off**: another singleton-ish piece of state. Mitigated by: scoped within the `streaming` namespace, never exposed publicly, lifetime gated by socketDisconnected. The raw `QWebSocket*` key is stable because Qt::deleteLater fires AFTER the disconnect slot returns, so the erase always runs before the socket is destroyed.

## 2026-05-15 WebSocket auth/session — pre-auth timeout now, RBAC + proxy + caps deferred

### Decision: 10s pre-auth socket timeout (close-after-no-AUTH)
- **Choice**: every accepted `QWebSocket` in `onNewConnection` gets a single-shot `QTimer(10000)` parented to itself. On fire, if `vms_authed` still false, close with `CloseCodePolicyViolated`. Timer auto-deletes with socket via Qt parent ownership.
- **Rationale**: the AUTH state machine was well-designed (JTI replay protection, IP binding, 30s ticket TTL, one-time use) but didn't bound the lifetime of an *unauthenticated* socket. Pre-fix an attacker could open N TCP+WS handshakes, never AUTH, and hold N file descriptors. Authentication design ≠ session resource policy.
- **Trade-off**: 10s is the choice point. Too short rejects clients on slow links / manual token paste; too long leaves the DoS window open. 10s matches the typical "client just opened WS, JS picking up the ticket from `/api/ws/ticket` HTTP response, sending AUTH frame" cycle which is <1s; legitimate slow links should still fit comfortably. Could become configurable later but a fixed 10s solves the immediate exposure.
- **Why a per-socket QTimer instead of a single sweeping QTimer**: per-socket timers are O(1) to set up + auto-cleanup-on-parent-delete. A central sweeper would need a registry, lookup-by-socket, and explicit removal on disconnect — more state for no benefit at our connection scale.

### Decision: Defer per-camera RBAC at WS subscribe — needs caching pass
- **Choice**: keep the current "any authed user subscribes to any camera_id" behaviour for now.
- **Rationale**: the `permissions` table HAS the `allowed_cameras` column and `PermissionRepository` HAS the SELECT helper, but nothing in the policy-enforcement layer reads it. Wiring it into the WS subscribe path requires (a) loading allowed_cameras at AUTH time or first subscribe, (b) caching it on the socket (Qt property or a side-map), (c) handling cache invalidation when an admin changes a user's permissions (currently no notify path), (d) deciding the "empty allowed_cameras = all cameras OR no cameras?" semantics (need product call). Each of those is its own design choice.
- **Trade-off**: until the RBAC sweep lands, a logged-in viewer can subscribe to any camera and see its frame stream. The HTTP `/api/cameras/*` endpoints might also bypass this — need a parallel audit. For deployments without per-user camera scopes (everyone-sees-everything), this is a non-issue. For multi-tenant deployments where operators expect viewer scopes, this is a real leak. Logged prominently in `past-bugs.md` as `BUG-WS-CAMERA-NO-RBAC-01 (HIGH)`.

### Decision: Defer trusted_proxies + connection caps + message-size cap + connection metrics
- **Trusted proxies**: X-Forwarded-For + localhost-bypass in IP binding combine to defeat IP-binding behind a reverse proxy. Right fix is a `config.security.trusted_proxies` list — only honour X-Forwarded-For if the immediate peer is in that list. Out of scope here because it touches `/api/ws/ticket` AND every other XFF-consuming endpoint.
- **Connection caps**: no per-IP or global cap on WS connection count. With the 10s pre-auth timeout, the immediate DoS vector is closed; reconnect-loop attacks still need a rate-limiter. Defer until the connection-count metric (see below) shows we need it.
- **Message-size cap**: `config.websocket.max_message_size_mb` is parsed but never applied to `QWebSocket::setMaxAllowedIncomingMessageSize` — Qt's default (~40 MB) is currently in effect. Tightening is a 1-line fix; logged but punted because the immediate AUTH timeout fix is enough for the security-lean scope.
- **Connection metrics**: mirror of the `delivery` + `batch_writer` counters landed earlier today. Need `connections_total / connections_current / authed_total / preauth_timeout_total / ticket_replay_total`. Right shape is a `CameraStreamManager::connectionStats()` accessor merged into `/api/rules/stats` (or a new `/api/ws/stats`). Defer to a dedicated observability pass.

## 2026-05-15 PipelineStateStore — shared_ptr-published frame, defer per-camera sharding

### Decision: Publish JPEG + objects as `shared_ptr<const vector<...>>`; copy outside the writer's unique_lock
- **Choice**: `PipelineStateSnapshot` stores `latest_frame_jpeg` and `latest_objects` as `std::shared_ptr<const ...>`. Writer (`updateFrame`) builds the new buffers via `std::make_shared` BEFORE acquiring the unique_lock, then under-lock only assigns the pointers + scalar fields. Readers under shared_lock copy the refcounted handle then drop the lock before deep-copying out (legacy API) or just return the handle (new `latestFrameJpegShared()` accessor).
- **Rationale**: pre-fix the unique_lock spanned the JPEG memcpy (200-500 KB) plus objects + metadata copy. At 30 fps × N cameras the write-side lock contention scaled linearly with camera count; every shared_lock reader on EVERY camera blocked behind a writer's memcpy on a SINGLE camera. The buffer copies are unavoidable — the writer's caller owns the source bytes only briefly, the store must take its own copy. But doing the copies UNDER the lock blocks unrelated work. Moving them out is a textbook fix.
- **Trade-off**: each frame allocates a fresh `shared_ptr` control block + vector buffer. At 1500 fps system-wide that's ~3000 small allocations/sec — modern allocators (mimalloc/tcmalloc/MSVC's segment allocator) handle this without contention. If profiling shows this as a bottleneck, switch to a per-camera ring of pre-allocated buffers reused across frames. Out of scope this pass.
- **Why not move the source buffer into the store**: the caller (`onFrameDecoded`) holds the JPEG bytes in a `vector<uchar>` local that's about to fall out of scope; if `updateFrame` took an rvalue we could avoid the copy entirely. But that signature change requires touching the call site + the QSignal-emitting NativeReaderWorker chain. The current "copy pre-lock" win captures most of the value without that ripple. Logged for a future zero-copy pass.

### Decision: Keep one global `shared_mutex`; defer per-camera sharding
- **Choice**: leave `PipelineStateStore::mutex_` as a single shared_mutex protecting all cameras' snapshots in one `unordered_map<int, PipelineStateSnapshot>`.
- **Rationale**: after the shared_ptr swap above, the writer's critical section drops to nanoseconds (pointer assignments + scalar field stores). Shared_lock readers parallelise with each other, and the writer's window is too small to meaningfully block them under typical loads. At 50 cameras × 30 fps the aggregate writer lock acquisition rate is 1500/sec — each acquisition is ~50 ns of work; readers see <0.01% blocked time.
- **Trade-off**: under extreme bursts (>200 cameras + write storms) the single mutex would still cap throughput. Real fix is per-camera sharding via `std::shared_ptr<PipelineStateSnapshot>` per entry with the map only guarding registry mutations. That's a bigger API rewrite — `snapshot()` becomes "look up the shared_ptr, drop the registry lock, dereference outside". Defer until the new `delivery` / `batch_writer` counters (landed same day in alert_delivery + DbManager audits) show pipeline updates contending.
- **Why not bucket-shard by `camera_id % N`?**: would work and is smaller surgery. But it leaks "we expected sharding to matter" into a struct that may not need it after the shared_ptr fix. Measure first.

### Decision: `latestFrameJpegShared()` is a NEW additive API; keep `latestFrameJpeg()` unchanged
- **Choice**: existing callers of `latestFrameJpeg(camera_id) → std::optional<std::vector<char>>` continue to work and continue to deep-copy on return. Add `latestFrameJpegShared(camera_id) → std::shared_ptr<const std::vector<char>>` as the zero-copy path for hot read sites (HTTP snapshot endpoint, WebSocket H.264 keyframe broadcast that bundles objects).
- **Rationale**: don't ripple this audit across every consumer right now. The big win is on the writer side; the read-side copy is a smaller cost that callers can opt into removing per-endpoint. Migrating callers also requires touching CameraPipelineManager's downstream API (it currently wraps the optional<vector> and returns it from `getLatestFrame()`). One pass per concern.

## 2026-05-15 DbManager hot-path — SAVEPOINT poison-row isolation, observability counters, defer transaction-mutex removal

### Decision: SAVEPOINT around per-row INSERT inside batched transaction (Postgres only)
- **Choice**: `flushEventBatch` now executes `SAVEPOINT vms_row_sp` before each `INSERT`, then either `RELEASE` on success or `ROLLBACK TO` on failure. SQLite path unchanged (per-statement errors don't abort the transaction). On Postgres, this isolates one bad row to that single statement so the rest of the batch still commits.
- **Rationale**: pre-fix the loop logged-and-continued on per-row exec failure, but Postgres MVCC marks the tx as aborted on first error and every subsequent INSERT fails. Commit then fails → the whole batch re-enqueues at front → infinite retry loop on the poisoned row. The "tolerant" code shape was correct for SQLite and silently broken for Postgres — this is exactly the dialect mismatch class that `.claude/rules/security.md` calls out as "states the new risk explicitly".
- **Trade-off**: the poisoned row is now DROPPED (not retried). Operators see `row_failures_total` increment + a WARN log per failure. Acceptable: a row that fails NOT NULL / type / CHECK violations is not data that future retry will heal — the schema or the producer is the problem. Re-enqueueing it is exactly the loop we're avoiding. If operators want failed-event archival, a "dead-letter table" is the right add (out of scope this pass).
- **Alternative considered**: switch to single-row tx-per-event (no batching). Rejected — multiplies SQLite WAL flush count by ~50× and Postgres tx overhead similarly. Batching is correct; per-row isolation is the missing piece.
- **Why not single shared savepoint name across all rows?**: a savepoint name is scoped to a transaction. Using `vms_row_sp` per-iteration is identical to using `vms_row_sp_<i>` per-iteration — Postgres's stack is bounded by the RELEASE/ROLLBACK at each iteration. Cleaner to keep one name.

### Decision: Six atomic counters + wait-free `batchWriterStats()` accessor
- **Choice**: `DbManager` gains `std::atomic<std::uint64_t>` for `enqueued_total / dropped_total / flushed_total / flush_failures_total / row_failures_total / peak_queue_depth`. `dropped_total` covers BOTH queue-full drops AND not-accepting-events drops (so the operator can distinguish backpressure from shutdown-race). `batchWriterStats()` is `const`, returns a POD snapshot, never blocks the producer.
- **Rationale**: same shape and rationale as the alert_delivery `BackgroundJobRunner` counters landed the same day. Event ingestion is the dominant write workload; if we can't quantify drops, we can't size queue/batch parameters or detect ingestion regressions. The cost is one relaxed atomic increment per event on a hot path already under `batch_queue_mutex_`.
- **Trade-off**: `batch_queue_mutex_` is now `mutable` so the `const` accessor can `try_to_lock` for `current_queue_depth`. We lose strict const-by-mutex but gain wait-free reads — the operator dashboard can poll every 5s without ever blocking event ingestion.

### Decision: Defer `transaction_mutex_` removal — document, don't touch
- **Choice**: keep the global `transaction_mutex_` that serializes `beginTransaction()` / `commit()` / `rollback()` across the 3 callers (zone save, rule save, ReID 60s flush) for now.
- **Rationale**: the mutex is named "DB-004 FIX: prevent interleaving" but with the current per-thread connection model (each thread gets its own `QSqlDatabase` via `getThreadConnection()` keyed on `std::this_thread::get_id()`), no two transactions share a connection — interleaving at the SQL level is already impossible. The mutex serializes them at the application level, which is unnecessary for both Postgres MVCC and SQLite WAL.
- **Trade-off**: actual production contention is near-zero (ReID flush every 60s × ~tens of ms hold, vs occasional rule/zone CRUD). Removing the mutex is the right cleanup but needs a careful pass: confirm no caller relies on the mutex for ordering (e.g. "I want my rule save to be visible before yours"), no DDL operation needs exclusive access, etc. The visibility counters landed this session let us measure the actual contention first; if it's nil, the removal is low-risk.
- **Why not remove now**: scope. The audit found the higher-priority bug (BUG-DB-PG-BATCH-ABORT-01 — actual data-loss risk on Postgres) and the visibility gap. Touching a global serializer is the kind of change that needs its own commit with its own bisect window.

### Decision: Defer SQLite WAL truncate policy — operator tuning > code change
- **Choice**: rely on SQLite's auto-checkpoint (1000 pages, PASSIVE) for now. Don't add an explicit `wal_checkpoint(TRUNCATE)` call.
- **Rationale**: under steady-state load with periodic reader gaps, auto-checkpoint completes and the WAL stays bounded. Problems arise only under sustained writes + constantly-active readers — at that point an operator-tunable PRAGMA settings block (size threshold + cadence) is more useful than a hardcoded cadence. Adding `wal_checkpoint(TRUNCATE)` in the batch writer idle path is a 5-line change but picks one policy for all deployments.
- **Trade-off**: WAL file may grow on a deployment that fits the bad profile. Memo flagged in past-bugs.md so operators see it during the next outage post-mortem; the right fix is a dedicated config knob.

## 2026-05-15 alert_delivery hot-path — push DNS + DB into workers; keep one shared pool (defer per-channel pool split)

### Decision: SSRF check (getaddrinfo) and DB settings reads move inside the worker job
- **Choice**: `deliverWebhook` / `deliverTelegram` / `deliverSMS` / `deliverAlarmOutput` now capture the operator-supplied URL + payload + already-resolved metadata fields on the producer thread, and run `isInternalUrl()` + `DbManager::getSetting()` inside the BackgroundJobRunner worker lambda. Producer thread does only cheap checks (URL prefix, non-empty recipient list, shutdown gate).
- **Rationale**: pre-fix every event matching a webhook rule paid a blocking `getaddrinfo()` on whichever thread invoked `EventManager::createEvent` — almost always the ZMQ bridge (single thread, drives ALL AI worker output) or a brand event service worker (one per camera). A dead resolver (5s timeout × 2 retries on Windows) becomes a 10s producer stall, and there is exactly one ZMQ bridge thread to share. The producer-side SSRF check was originally there to "refuse before queueing" — but queue capacity (128 slots, drop-on-full) is cheap; producer thread time is expensive. Worker-side refusals just consume a few microseconds of the lambda before returning.
- **Trade-off**: a worker slot is briefly held on every SSRF-refused job. With 2 workers and 128 queue, even a 100% refusal burst is ~64 jobs/sec × tiny work = sub-millisecond aggregate. Net positive vs producer stall.
- **Alternative considered**: per-rule URL DNS cache (TTL'd `unordered_map<string, ResolveEntry>`) on the producer side. Rejected for this pass — amortises only on repeat-same-URL bursts and adds a shared map + mutex on every dispatch. Right tool only if profiling proves >100 evals/sec on one rule.

### Decision: One shared BackgroundJobRunner pool, defer per-channel pool split
- **Choice**: keep the existing single 2-worker / 128-queue `deliveryRunner()` for ALL channels (webhook + SMS + Telegram + alarm-output) instead of splitting per channel class.
- **Rationale**: pool split is the architecturally right fix for the cascade-failure mode (one stalled webhook stops Twilio + Telegram + alarm relay even though those endpoints are healthy). But the split needs design work — how many pools, sizing, shared-vs-isolated workers, draining policy on shutdown — and naively bumping to 4 pools = 8 threads ambient + complicates `shutdownDelivery`'s join sequence. Moving DNS + DB off the producer thread closes the dominant production failure mode (producer stall blocking event ingestion). Cascade between channels matters at the channel-saturation point, not the event-ingestion point.
- **Trade-off**: under a sustained burst with a dead webhook, the 2-worker pool still serially times out at 15s each, queueing Twilio/Telegram/email behind. Until split lands, operators mitigate by trimming webhook URLs to known-fast endpoints; new `deliveryStats()` surface exposes `queue_depth` + `dropped_total` so the symptom is visible (was previously invisible — see BUG-ALERT-DROP-VISIBILITY-01).
- **Why not raise workers to 4 or 8 now**: more workers help only if the bottleneck is parallelism; here the bottleneck is one endpoint × 15s timeout. Two stalled jobs still stall 2 workers; eight workers stall 8 just as well. Real fix is isolation, not parallelism. Punt until isolation lands.

### Decision: Drop visibility = atomic counters on BackgroundJobRunner, exposed via stats endpoint
- **Choice**: `BackgroundJobRunner` gains three `std::atomic<uint64_t>` counters (`submitted_total`, `dropped_total`, `peak_queue_depth`) and a `current_queue_depth` derived via `jobs_.size()` snapshot. New `stats()` accessor returns a POD struct; `alert_delivery::deliveryStats()` wraps it to JSON; exposed on `GET /api/rules/engine/stats` (admin scope, joins existing RuleEngine `getStatistics()` output).
- **Rationale**: pre-fix the only signal of dropped alerts was a throttled WARN log line every 5s. Operators cannot post-hoc answer "did we lose alerts during yesterday's burst?" Counters give them an exact answer + a deltas-over-time series via repeated polls. Atomic loads are wait-free, no contention with the producer.
- **Trade-off**: 24 bytes of state per BackgroundJobRunner instance + 2 atomic-increments per submit (lock-amortised; already under queue mutex). Cost-free in steady state.

## 2026-05-14 ReID gallery persistence — periodic flush, not per-mutation write

### Decision: 60s dirty-flag flush thread, not per-mutation SQL writes
- **Choice**: ReIDEngine::loadFromDatabase starts a background `std::thread` that wakes every 60s (or earlier via `condition_variable` on shutdown). It calls `saveToDatabase()` only when `persistence_dirty_` (`std::atomic<bool>`) is true; that flag is set by `processDetection` / `clearGallery` / `pruneExpired`. Worst-case data loss is 60s of identities + trail updates.
- **Rationale**: Per-detection persistence at peak load (~30 detections/s across cameras) would be ~30 SQL transactions/s writing 2 KB of embedding BLOB each — measurable disk pressure and lock contention with the existing event batch writer. Dirty-flag + periodic flush is the same shape as `CounterBucketAggregator` (60s rollup) and `EventManager` (batch writer) — consistent with the codebase's "no synchronous DB write on the hot path" rule.
- **Trade-off**: A process kill -9 between flushes loses up to 60s of mutations. Crash detection scenario is real (NSSM auto-respawn from BUG-PM-RESTART-01 sequence) but the lost data is recoverable on the next observation: the same person walks past the same camera and gets a new global_id — operationally similar to the pre-fix state of every restart, just bounded to 60s window. If we ever care about graceful-shutdown loss only (kill -9 acceptable), bump flush to 5 minutes and document operator expectation.
- **Alternative considered**: write-on-mutate with a debouncer (e.g. 250ms grace before flushing). Rejected because (a) implementing debounce correctly requires the same thread + cv + dirty-flag plumbing, just with shorter interval, and (b) the 250ms grace would still be fully busy under burst load and lose the batch benefit.

### Decision: TTL filter at load time, not load-then-prune
- **Choice**: `loadFromDatabase` runs `SELECT … WHERE last_seen >= ?` with cutoff = `now() - gallery_ttl_sec`. Stale rows never enter memory.
- **Rationale**: A multi-hour downtime (NSSM stopped over weekend) restarting with the previous gallery would briefly produce wrong cross-camera matches before the next `pruneExpired()` fired — even if just 1-2 stale identities, the operator confidence cost is real. Filter at load is one SQL parameter; filter at memory-load-then-prune is two operations + a window of wrong behavior.
- **Trade-off**: SQL needs an index on `last_seen` to keep the filter fast at scale. Added (`idx_reid_last_seen`).

### Decision: Embeddings stored as raw float32 BLOB, not JSON array or base64
- **Choice**: `INSERT INTO reid_gallery (... embedding ...)` binds a `QByteArray` containing the raw `sizeof(float) * embedding_dim` bytes. Sidecar `embedding_dim` column lets the reader validate the BLOB size matches what's expected before reinterpret_cast.
- **Rationale**: BLOB is 4× smaller than base64 text (2 KB vs 8 KB per row at 512-dim), no encode/decode CPU, and Qt SQL handles BYTEA + BLOB transparently. JSON array would be 5-6× the size and force per-element parse. The portability concern (operator inspecting embeddings via `sqlite3` CLI) is moot — embeddings aren't human-readable in any encoding.
- **Trade-off**: `embedding_dim` column adds 4 bytes/row of duplicated info (could be derived from blob size / 4). Cheap and catches the silent-malformed-blob case explicitly.

### Decision: Trails use DELETE-then-INSERT, not UPSERT
- **Choice**: Each flush deletes all `reid_trails` rows for active gids and re-inserts from the in-memory vector. Bounded by ~5000 rows (max_gallery=500 × avg trail length ~10).
- **Rationale**: TrailPoint has no natural unique key per row (gid+camera_id+enter_time would work but assumes no re-entry; a person walking through cam 3 → cam 5 → cam 3 again would produce two cam-3 points with different enter_times — UPSERT would need a synthetic id and the in-memory `std::vector` doesn't carry one). DELETE-then-INSERT is exact-match-the-in-memory-state, no edge cases. Bounded size makes the IO trivial.
- **Trade-off**: 1 DELETE + N INSERTs per flush even when only one trail changed. Acceptable at the 60s cadence. If trail mutation rate ever becomes the bottleneck, switch to "append new points only" (track a high-water mark per gid).

## 2026-05-14 AlertManager consolidation — single-layer rule dispatch via deliverAction

### Decision: One delivery layer (`vms::events::deliverAction`), not two rule storage layers
- **Choice**: Delete `AlertManager` (legacy `alert_rules` table consumer) AND `AlertRouter` (modern in-memory rule store that was never populated). Add `vms::events::deliverAction(rule, action, event)` that takes a `CompositeRule` + `RuleAction` directly and dispatches each `action.channels` entry via the existing helpers (`vms::utils::sendEmailAsync`, libcurl with SSRF guard, Twilio HTTPS, Telegram, alarm relay). Per-channel recipients live in `RuleAction.metadata` as `email_addresses[]` / `phone_numbers[]` / `telegram_chat_id` (the schema field is already `nlohmann::json` so no DB migration needed for new fields).
- **Rationale**: Pre-consolidation the codebase had three rule layers (legacy SQL `alert_rules`, modern SQL `rules` → RuleEngine, AlertRouter in-memory) and exactly one of them delivered (the legacy). RuleEngine ALERT/WEBHOOK actions routed to AlertRouter::routeEvent which iterated AlertRouter's rules_ list — that list had zero writers in production code, so every modern rule was a no-op delivery despite recording trigger logs. The 2026-05-12 dispatch-bypass fix made this latent bug actively bite (every brand-camera + ZMQ event now flowed through both layers; legacy fired, modern still silent). Wiring AlertRouter to load from the same `rules` table would have produced two parallel rule stores; the right factoring is no AlertRouter at all — RuleEngine evaluates conditions, picks rules, hands each matching rule + action to deliverAction.
- **Trade-off**: One legacy REST surface (`GET/POST/DELETE /api/alerts/rules`) deleted. Frontend had zero callers (confirmed via grep). External integrations using the legacy POST would 404 after the upgrade — accepted because (a) those rules are migrated to the new format on first boot, (b) `/api/rules` is the canonical surface, (c) maintaining a backward-compat shim that proxies into the new schema is busy-work for an interface nobody verified in use.
- **Why not just remove the legacy and leave AlertRouter**: AlertRouter would still need rules loaded from somewhere. The cleanest source is the `rules` table, but then we'd have RuleEngine AND AlertRouter both reading the same table and applying conditions/rate-limits — double work + double mutex traffic on every event. Reasonable in a system with composite rules at scale, but the cost/benefit doesn't justify it at current rule counts (<100). RuleEngine already does anti-noise (cooldown + debounce); AlertRouter's `max_per_hour` rate-limit is the one thing not replicated — flagged as a followup (`CompositeRule.anti_noise.max_alerts_per_hour` exists but is currently unenforced; wiring it up is a 10-line change once needed).

### Decision: One-shot boot migration with idempotency flag, NOT a backward-compat REST shim
- **Choice**: `RuleEngine::migrateLegacyAlertRules()` runs at boot after `loadFromDatabase`, gated by the `alert_rules_migrated` setting. Each legacy row → one `CompositeRule` with EVENT_TYPE condition (if not '*') + camera scope + ALERT/WEBHOOK action carrying the recipient in `metadata`. Persists via `saveToDatabase`. Sets the flag to "1". On subsequent boots, short-circuits.
- **Rationale**: Customers with existing legacy rows would silently lose all alert delivery on first deploy of the post-consolidation backend if migration was manual. A boot-time migration is invisible-when-empty (no legacy rows = no-op + flag set) and self-healing (a partial migration retries on next boot because the flag is only set after `saveToDatabase` succeeds). A REST shim that proxied `/api/alerts/rules` POST into the new schema would have had the same effect but kept a dead endpoint alive forever — a future audit pass would correctly flag it as unused and try to delete it, walking us back into this same conversation.
- **Trade-off**: The `alert_rules` CREATE TABLE remains in `db_manager.cpp` (idempotent `IF NOT EXISTS`) so the legacy table still exists post-migration. Operators can `DROP TABLE alert_rules` manually if they care; we don't drop it automatically because the table itself is operator-visible historical data and dropping a table at boot is a louder action than leaving an empty table. Re-running the migration is impossible without manually clearing the flag — accepted because the migration is idempotent w.r.t. data shape (an addRule on an already-migrated row would create a duplicate, but the flag prevents it).

### Decision: AlertChannel enum moves into `events/rule_engine.h`, not its own header
- **Choice**: Inline the enum + helpers at the top of `rule_engine.h`. Previously they lived in `events/alert_router.h`, which `rule_engine.h` re-exported via include.
- **Rationale**: `RuleAction.channels` is the only consumer. Splitting into a third header (`events/alert_channel.h`) would add a file with one enum and two free functions; including in rule_engine.h keeps the file count constant relative to before consolidation (we still deleted 2 headers and 2 cpps net), and matches where the data structure that uses it lives. The trade-off is rule_engine.h grows by ~20 lines.

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
