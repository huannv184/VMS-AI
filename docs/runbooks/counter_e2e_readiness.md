# Counter — End-to-End Readiness Runbook

**Purpose**: hardware-side verification that the line-crossing counter pipeline
works as designed after PR-1 (commit `1058ef0`). The unit tests in
`test_counter_summary.cpp` cover the controller math; this runbook covers the
chain only real hardware can prove: camera → person detection →
`LINE_CROSSING_*` event → `counter_buckets_1m` → `/api/counter/summary` → UI.

**Who runs this**: ops engineer on a deployment with at least one functioning
camera and the AI pipeline running.

**How long**: ~5–10 min once a counting line is drawn.

---

## Prerequisites

1. At least one camera in the system is actively streaming and running person
   detection. Confirm via `GET /api/cameras/<id>/metadata` — should return
   objects with `class_id=0` (person) when someone is in the frame.
2. Operator can walk into / out of the camera's field of view.
3. Operator has CounterView open in the UI (`/counter` route) and can also hit
   the `/api/counter/*` endpoints with the operator role (ANALYTICS_READ).

---

## Step-by-step

### 1. Aggregator is running

```
GET /api/counter/status
```
Expected:
```json
{
  "aggregator": { "running": true, "interval_seconds": 60, "lookback_minutes": 5, ... },
  "tracker":    { "cache_loaded": true, "total_lines": 0 }
}
```
- ✅ `aggregator.running` is `true` → background sweep thread is alive.
- ⚠️ If `false`: backend boot failed to start the aggregator. Tail the backend
  log for `CounterBucketAggregator: started` line. If absent, restart
  `vms_backend` and re-check.

### 2. Draw a counting line on the camera

In CounterView → "Quản lý vạch đếm" → pick the camera → "Thêm vạch". Draw a
line across a normal walking path (door, corridor center). Save.

```
GET /api/counter/lines?camera_id=<id>
```
- ✅ Returns the line you just drew with `enabled: true`.

```
GET /api/counter/status
```
- ✅ `tracker.total_lines` increments to at least 1.

### 3. UI badge state at this point

CounterView header badge should show:
```
Aggregator hoạt động · N vạch
```
(green dot). If it shows `Chưa cấu hình vạch đếm` after step 2, hit `Làm mới`
or wait up to 15s for the next poll.

### 4. Generate a crossing

Walk through the camera's FOV, deliberately crossing the line in one direction
(say A→B). Wait ~2 seconds at the far side so the tracker confirms the cross.

### 5. The event landed in `events`

```sql
SELECT id, camera_id, event_type, timestamp, metadata_json
FROM events
WHERE event_type LIKE 'LINE_CROSSING_%'
  AND camera_id = <id>
ORDER BY id DESC LIMIT 5;
```
Expected:
- ✅ At least one row with `event_type` = `LINE_CROSSING_A_TO_B` or `LINE_CROSSING_B_TO_A`.
- ✅ `metadata_json` contains `"line_id": <your line id>`.

⚠️ If no row appears within 5s of crossing:
- Person not detected: confirm AI worker actually emitted `class_id=0` in
  `cameras/<id>/metadata`. If not, the upstream detection is the problem, not
  the counter.
- Line drawn outside the actual walking path: verify the line geometry in the
  drawing editor against a recent snapshot.
- Multi-frame confirmation gate: if `VMS_INTRUSION_CONFIRM_FRAMES > 1` is set
  in env, the crossing needs `N` consecutive frames before firing. Walk slowly.

### 6. Aggregator sweep picks it up

Wait up to 60s (the default `interval_seconds`). Then:

```
GET /api/counter/status
```
- ✅ `aggregator.total_sweeps` incremented since step 1.
- ✅ `aggregator.total_upserted` incremented (≥ 1).
- ✅ `aggregator.last_sweep_ms` is non-zero.

### 7. `/api/counter/summary` shows the cross

```
GET /api/counter/summary?camera_id=<id>
```
Expected (today, local midnight → now):
```json
{
  "camera_id": <id>,
  "total_in": 1 or 0,
  "total_out": 1 or 0,
  "total_today": 1,
  "peak_hour": <local hour>,
  "lines": [ { "line_id": <id>, "in_count": ..., "out_count": ... } ]
}
```
The split between `in_count` and `out_count` follows the convention
`LINE_CROSSING_B_TO_A → in_count`, `LINE_CROSSING_A_TO_B → out_count`. If your
"entry" direction maps differently, configure `direction_a_label` /
`direction_b_label` on the line.

### 8. UI reflects the count

In CounterView, the "Tổng vào hôm nay" / "Tổng ra hôm nay" / "Lưu lượng tổng"
big-number tiles should show the new value within one fetch cycle (auto on
mount, or click `Làm mới`).

---

## Troubleshooting matrix

| Symptom | Likely cause | Where to look |
|---|---|---|
| `/status.aggregator.running = false` | Boot failure | backend log `CounterBucketAggregator: started` |
| `/status.tracker.total_lines = 0` after drawing | Line save failed silently | inspect POST `/api/counter/lines` response |
| `events` has rows but `/summary` returns 0 | `metadata_json` missing `line_id` | aggregator skips malformed rows; check the event you generated |
| `/summary` non-zero but UI shows 0 | FE pointing at stale endpoint | confirm `apiClient.getCounterSummary` is called (not `getTrafficSummary`) — should be after PR-1 |
| Badge stuck on `Aggregator dừng` | Boot failure OR aggregator panic | backend log + restart |
| Counts way higher than crossings | Multi-tracking same person | `VMS_COUNTER_CROSSING_COOLDOWN_MS` env (default 3000) |

---

## What this runbook does NOT prove

- Production load behaviour (this is a single-camera, single-operator test).
- Accuracy at distance / partial occlusion / low light.
- Long-running drift in `counter_buckets_1m` aggregation.

For those, a synthetic load test driving ZMQ events is the right tool — see
PR-7 follow-up if scoped.
