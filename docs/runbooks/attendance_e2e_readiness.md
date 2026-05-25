# Attendance — End-to-End Readiness Runbook

**Purpose**: hardware-side verification of the attendance pipeline after PR-5
(commit `6f1f0da`). Closes the operational audit gap: unit tests cover the
health-classification matrix; this runbook covers the chain only real face
recognition + employee config + door-role config can prove.

**Who runs this**: ops engineer with face DB populated (at least 1–2 known
people), at least one camera, and the attendance feature being deployed for
the first time or after a config change.

**How long**: ~10 min for the happy path, +5 min for each degraded-state test.

---

## Prerequisites

1. Face DB has at least one enrolled person. Verify via `GET /api/faces/persons`
   → response has at least one row with a non-empty `embedding`.
2. AI worker is running and recognizing faces. Test by walking past a camera —
   `GET /api/events?event_type=FACE_RECOGNIZED&limit=5` should show recent
   rows with the right `person_id`.
3. AttendanceSettings open in UI (under settings or admin nav).
4. Operator has both an **enrolled** person and an **unenrolled** person
   available (the unenrolled one is the negative-control for the degraded
   path in §5).

---

## Step-by-step

### 1. Tracker is started

```
GET /api/attendance/health
```
Expected at boot, before anyone walks past:
```json
{
  "health": "ok",
  "started": true,
  "pending_rows": 0,
  "dropped_rows": 0,
  "employees_cached": <N>,
  "camera_roles_cached": <M>,
  "last_event_ts": 0,
  "seconds_since_last_event": -1,
  "stale_after_s": 3600,
  "unlinked_events_24h": 0,
  "fallback_events_24h": 0
}
```
- ✅ `started: true`
- ✅ `employees_cached` reflects rows in `employees` table (run
  `SELECT COUNT(*) FROM employees WHERE active=1` to compare)
- ✅ `camera_roles_cached` reflects rows in `camera_roles`
- `health: "ok"` and `last_event_ts: 0` together are **expected** at fresh
  boot — see "Why ok when there's no activity?" below.

⚠️ If `started: false`: tracker failed to start at boot. Tail backend log for
`AttendanceTracker` lines. Restart backend if absent.

### 2. Configure the happy path

For the verification person:
1. They're enrolled in face DB (have a `persons.id`).
2. There's an `employees` row mapping that `person_id` to an employee record.
   - Add via AttendanceSettings → "Danh sách nhân viên" → "Thêm" if missing.
3. The camera they'll walk past has a role set.
   - Set via AttendanceSettings → "Vai trò camera" → choose `entry` for an
     entry-door camera, `exit` for exit, `both` for toggling, or `observe`
     for monitoring-only.

After config:
```
GET /api/attendance/health
```
- ✅ `employees_cached` incremented if you added a new employee
- ✅ `camera_roles_cached` incremented if you added a new role
- Cache updates on the next CRUD call — no restart needed.

### 3. Enrolled person walks past

The enrolled person walks past the configured camera, looking at it for ~1s
to give SCRFD + ArcFace a clear shot.

### 4. Recognition lands as attendance

```sql
SELECT id, person_id, employee_id, camera_id, kind, timestamp, source_rule
FROM attendance_events
ORDER BY id DESC LIMIT 5;
```
Expected within ~3s of the walk-past:
- ✅ Row with `person_id` matching the enrolled person.
- ✅ `employee_id` is **NOT NULL** (matches the employee row you confirmed in §2).
- ✅ `kind` is `in` / `out` / `seen` depending on the camera role.
- ✅ `source_rule = 'door_role'` (NOT `'min_max_fallback'`).

```
GET /api/attendance/health
```
- ✅ `last_event_ts` non-zero, recent.
- ✅ `seconds_since_last_event` is single digits.
- ✅ `unlinked_events_24h = 0` and `fallback_events_24h = 0` (because the
  config was complete).
- ✅ `health: "ok"`.

UI badge in AttendanceSettings: green `Hoạt động · <N> NV / <M> camera`.

⚠️ If `attendance_events` has no new row after 10s:
- Recognition didn't fire — verify a `FACE_RECOGNIZED` event landed in
  `events` table for the same timestamp. If absent, the upstream is the
  problem, not attendance.
- Recognition fired but with `person_id <= 0` (unknown face) — those are
  dropped by design at `AttendanceTracker::onFaceRecognized` line 147.
- Dedup window — if the same person was recognized within the last 60s
  (default `VMS_ATTENDANCE_DEDUP_SECONDS`), the second attempt is silently
  dropped.

### 5. Degraded path A: enrolled person with no employee mapping

The whole point of PR-5's audit endpoint is to surface this case.

1. Take an enrolled person who is **NOT** linked to an employee (or temporarily
   delete the link via Employees → delete row).
2. They walk past the configured camera.
3. `attendance_events` gets a row but `employee_id = NULL`.
4. Within 10s:
   ```
   GET /api/attendance/health
   ```
   - ✅ `unlinked_events_24h` incremented by 1.
   - ✅ `health: "degraded"`.

UI:
- Badge in AttendanceSettings turns yellow: `Đang chạy — cấu hình thiếu`.
- A warning banner appears below the health row:
  > Cấu hình chấm công có lỗ hổng (24h qua) — N recognition có person_id
  > nhưng chưa map sang nhân viên — ghi vào DB với employee_id = NULL, không
  > vào báo cáo. Vào mục Danh sách nhân viên bên dưới để bind.

5. Fix: bind person → employee in the table directly below. On next poll
   (~10s) the banner stays until the 24h window rolls past the bad events;
   `health` flips back to `ok` after the count returns to 0. (Banner does NOT
   auto-hide before the window expires — by design, so operator confirms
   they've cleared the backlog of unlinked rows.)

### 6. Degraded path B: enrolled person on a camera with no role

1. Configure an employee mapping (so this isn't path A).
2. Set the test camera's role to **none** (delete its row from `camera_roles`
   via AttendanceSettings).
3. Enrolled person walks past.
4. `attendance_events` row has `source_rule = 'min_max_fallback'`.
5. Within 10s:
   ```
   GET /api/attendance/health
   ```
   - ✅ `fallback_events_24h` incremented by 1.
   - ✅ `health: "degraded"`.

UI: banner shows the fallback message with a hint pointing at "Vai trò camera".

### 7. Stale path (optional)

If `VMS_ATTENDANCE_STALE_S` is set short (e.g. `300` for testing) AND
`last_event_ts > 0` from prior activity, wait past the cutoff with no
face-recognition activity:
```
GET /api/attendance/health
```
- ✅ `health: "stale"` once `seconds_since_last_event > stale_after_s`.

For production, the default 3600s (1h) is intentionally long because face
recognition is sparse; quiet hours are expected.

---

## Why `ok` when there's no activity?

Backend distinguishes `last_event_ts == 0` (never had an event since boot)
from `last_event_ts > 0` + old (had events, now silent). The former is
**not** stale — fresh boot, waiting for first activity, normal. Only the
latter triggers `stale` after the cutoff. See test
`OkIgnoresZeroLastEventEvenAfterCutoff` in `test_attendance_health.cpp`.

---

## Troubleshooting matrix

| Symptom | Likely cause | Where to look |
|---|---|---|
| `started: false` at boot | Tracker initialization failed | backend log `AttendanceTracker:` lines |
| `employees_cached: 0` but employees table has rows | `reloadEmployees` not called or DB connection failed | trigger `POST /api/attendance/employees/<any>` (refreshes) or restart |
| Face recognized but no attendance row | `person_id <= 0`, dedup window, or tracker stopped | check `events.metadata_json.person_id` for the recognition event |
| `employee_id` always NULL despite mapping | `emp_by_person_` cache stale | hit `POST` or `PUT` on `/api/attendance/employees/...` to trigger reload |
| `source_rule` always `min_max_fallback` | `camera_role_` cache empty | check `SELECT * FROM camera_roles`; if non-empty, force reload via POST `/api/attendance/camera-roles` |
| Badge stays loading forever | `/health` returning non-200 | network tab in browser; check backend log for the route |
| Banner won't clear | Window not yet rolled past the bad events | banner is by-design persistent for the 24h window; fix the config and wait |

---

## What this runbook does NOT prove

- Face recognition accuracy (false matches / missed matches). That's an AI
  worker concern, see `face_reid_audit_2026_05_08.md` memory.
- Night-shift / overnight rollup correctness (covered in unit tests at
  `test_attendance_shift.cpp` after `night_shift_fix_2026_05_04`).
- Long-running drift of `last_event_ts` across day boundaries.
- Concurrent load: 100+ simultaneous walk-ins, BulkWriter backpressure
  behaviour under sustained write rate.

For load + drift, a synthetic test fixture pumping `onFaceRecognized` calls
at production rate against an in-memory sqlite is the right tool. Out of
scope for this runbook; track as a PR-7 follow-up.
