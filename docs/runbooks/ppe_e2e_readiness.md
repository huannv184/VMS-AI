# PPE — End-to-End Readiness Runbook

**Purpose**: hardware-side verification that the PPE compliance pipeline works
as designed after PR-2 (commit `bbf79f1`). Closes the "phantom feature" review
class: the unit tests cover the health-classification matrix; this runbook
covers the chain only a real ai_worker + PPE engine + real person without
helmet/vest can prove.

**Who runs this**: ops engineer with a deployment where the PPE engine binary
has been deployed (`PPE-YOLOv8m.engine`) and at least one camera will be
configured to run PPE inference.

**How long**: ~10 min including warm-up.

**Important caveat**: backend cannot distinguish "ai_worker loaded engine and
scene is empty" from "ai_worker silently failed to load engine". Both look
like `inactive` from `/ppe/status`. This runbook tells you how to disambiguate
by triggering a known violation.

---

## Prerequisites

1. `PPE-YOLOv8m.engine` deployed next to the `ai_worker` binary.
2. At least one camera that will run PPE. Two ways to enable:
   - Per-camera: edit camera in CameraConfigModal, set `ai_config.ppe = true`.
   - Process-wide: set env `VMS_AI_ENABLE_PPE=1` before starting the backend.
3. A person available who can stand in the camera's FOV both **with** and
   **without** a helmet/vest (helmet alone is the easiest test — the engine
   detects 6 classes: helmet/vest/person/gloves/mask/boots).
4. PpeMonitorView open in UI (`/ppe` route).

---

## Step-by-step

### 1. Configuration probe sees the intent

```
GET /api/analytics/ppe/status
```
Expected after configuring at least one camera with `ai_config.ppe=true`:
```json
{
  "health": "inactive",
  "config": {
    "enabled_cameras": [<your camera id>],
    "enabled_count": 1,
    "env_force_enable": false,
    "env_force_disable": false
  },
  "runtime": {
    "active_cameras": 0,
    "last_tick_ms": 0,
    "seconds_since_last_tick": -1,
    ...
  }
}
```
- ✅ `config.enabled_cameras` includes your camera id.
- `health: "inactive"` is **expected** at this point — no PPE ticks yet.

⚠️ If `config.enabled_count = 0`:
- The `ai_config` JSON on the camera row doesn't have `"ppe": true`. Inspect
  via `SELECT ai_config FROM cameras WHERE id = <id>`. Re-save via the modal.

### 2. Walk into FOV WITH a person (compliance baseline)

Stand in the camera's FOV wearing a helmet (or any PPE — the goal here is to
prove the engine fires, not that you're compliant). Wait ~5–10s.

### 3. PPE telemetry arrives

```
GET /api/analytics/ppe/status
```
Expected within ~10s of standing in FOV:
- ✅ `runtime.active_cameras >= 1`
- ✅ `runtime.last_tick_ms > 0` and `seconds_since_last_tick` is small (1–10s)
- ✅ `health` flips from `inactive` → `ok`

UI badge in PpeMonitorView should show:
```
PPE hoạt động · 1/1 cam
```
(green). Hover the badge to see tick counts in the title attribute.

⚠️ If `last_tick_ms` stays `0` after 30s of standing in frame:
- ai_worker did NOT load the PPE engine. Tail ai_worker log for:
  - `[AI-Worker-<id>] VMS_AI_ENABLE_PPE=1 — ppe ON` (env path)
  - PPE engine init line (load success / failure)
- Check the engine file exists and has the right name (`PPE-YOLOv8m.engine`).
- Confirm camera was started after the env / config change.

### 4. Trigger a real violation (the actual test)

Remove the helmet. Stand in FOV without one for ~3–5s.

### 5. Violation lands

```sql
SELECT id, camera_id, event_type, label, timestamp
FROM events
WHERE event_type = 'PPE_VIOLATION'
  AND camera_id = <id>
ORDER BY id DESC LIMIT 5;
```
Expected:
- ✅ Row with `event_type = 'PPE_VIOLATION'`.
- ✅ `label` indicates the missing class (e.g. `NoHelmet`, `NoVest`).

```
GET /api/analytics/ppe/status
```
- ✅ `runtime.last_violation_ms > 0`
- ✅ `seconds_since_last_violation` is small

```
GET /api/analytics/ppe_compliance?window_minutes=10
```
- ✅ `global.compliant_ticks > 0` AND `global.violating_ticks > 0`
- ✅ `compliance_rate` between 0 and 1 (closer to 0 if you spent more time
  without the helmet)

### 6. UI surfaces the violation

PpeMonitorView:
- "Vi phạm hôm nay" tile shows count ≥ 1
- "Tỷ lệ tuân thủ" tile shows a real percentage (not `—`)
- The violation appears in the "Danh sách vi phạm gần đây" table

### 7. Stale path (optional, slower test)

Stop the ai_worker (or stop the camera). Wait `VMS_PPE_STATE_STALE_S` seconds
(default 300 / 5 min).

```
GET /api/analytics/ppe/status
```
- ✅ `health: "stale"`
- ✅ Badge shows `PPE im lặng Ns` with red color
- Restart ai_worker → badge returns to `ok` within ~30s of next tick.

---

## Troubleshooting matrix

| Symptom | Likely cause | Where to look |
|---|---|---|
| `health: "no_cameras"` | No camera has `ai_config.ppe=true` and env not set | configure via modal OR set `VMS_AI_ENABLE_PPE=1` |
| `health: "inactive"` after 30s+ in FOV | Engine didn't load | ai_worker log — engine init line |
| Person detected but no `ppe_summary` | Engine loaded but produces zero output | ai_worker log for PPE inference errors |
| Compliance rate stuck at 100% despite no helmet | Engine class mapping mismatch | check `ai.ppe.{helmet,vest,...}_class` in `backend.yaml` against engine's actual class indices |
| Badge says active but compliance shows `—` | `recordTick` called with both counts = 0 | rare; check ai_event_processor.cpp:191 PPE-summary branch |
| `health: "stale"` during work hours | Engine crashed or all PPE cams offline | ai_worker log + restart |

---

## What this runbook does NOT prove

- Engine accuracy (false positives / false negatives at distance, partial
  occlusion, motion blur). For tuning thresholds see
  `ppe_all5_enforcement_2026_05_21.md` memory; engine quality is a model
  problem, not a pipeline problem.
- Multi-camera concurrent load (the rolling 60-min ring is small but the
  recordTick mutex is single).
- Long-running soak (does `last_tick_ms` stay correct across day boundaries).

For load + soak verification, simulate ZMQ PPE traffic at production rate
against a backend with no real ai_worker connected. Out of scope for this
runbook; track as a PR-7 follow-up if scoped.
