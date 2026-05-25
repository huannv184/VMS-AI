# Ops Checklist — E2E Hardware Verification

Quick-reference checklist for the per-module runbooks
(`counter_e2e_readiness.md` / `ppe_e2e_readiness.md` /
`attendance_e2e_readiness.md`). Tick each item during execution; use the
companion `e2e_verification_report_template.md` to capture evidence.

Treat any **un-ticked** item as a blocker for declaring the module
production-ready on this deployment. Use the troubleshooting matrix at the
end of each per-module runbook to diagnose failures.

---

## Counter

- [ ] Camera stream lên ổn định, person detection xuất hiện trên camera test.
- [ ] Có counting line cấu hình đúng camera.
- [ ] Đi qua line theo hướng A/B tạo được event `LINE_CROSSING_*`.
- [ ] `GET /api/counter/status` trả `aggregator.running=true`, line đã load (`tracker.total_lines > 0`).
- [ ] `GET /api/counter/summary` hoặc `GET /api/counter/history` trả count > 0 sau khi đi qua line.
- [ ] UI CounterView hiển thị số tăng tương ứng (Tổng vào/Tổng ra/Lưu lượng tổng).
- [ ] Chụp lại 1 screenshot UI + 1 API response (summary hoặc history) + 1 log snippet (`CounterBucketAggregator: sweep OK` lines).

## PPE

- [ ] PPE engine/model load thành công khi boot (ai_worker log có dòng init engine, không có "engine load failed").
- [ ] `GET /api/analytics/ppe/status` trả `health: "ok"` (hoặc tối thiểu `inactive` chuyển sang `ok` khi có người).
- [ ] Người mang đủ PPE không sinh `PPE_VIOLATION` bất thường (`compliant_ticks` tăng, `violating_ticks` đứng yên).
- [ ] Người cố ý bỏ helmet hoặc vest sinh `PPE_VIOLATION` đúng loại (`label` = `NoHelmet`/`NoVest`/...).
- [ ] `runtime.last_violation_ms` cập nhật, `health` vẫn `ok` (vi phạm không kéo về `inactive`/`stale`).
- [ ] UI PpeMonitorView badge phản ánh đúng trạng thái mới ("PPE hoạt động · N/M cam"), bảng violation hiện entry mới.
- [ ] Chụp lại screenshot UI + API response (ppe/status + ppe_compliance) + log snippet (ai_worker PPE tick log).

## Attendance

- [ ] Face recognition nhận diện được người đã đăng ký (event `FACE_RECOGNIZED` xuất hiện trong `events` table với `person_id` đúng).
- [ ] Với người đã map employee, đi qua camera tạo được row trong `attendance_events` với `employee_id` NOT NULL và `source_rule='door_role'`.
- [ ] `GET /api/attendance/health` hiển thị `started=true` và `last_event_ts` cập nhật.
- [ ] UI AttendanceSettings badge xanh ("Hoạt động · N NV / M camera"); rollup hiển thị dữ liệu.
- [ ] Với người **chưa map** employee, `unlinked_events_24h` tăng + warning banner xuất hiện ("Cấu hình chấm công có lỗ hổng").
- [ ] Với camera **chưa có role**, `fallback_events_24h` tăng + banner mention `min_max_fallback`.
- [ ] Chụp lại screenshot UI (badge + banner) + API response (health) + DB row (`SELECT * FROM attendance_events ORDER BY id DESC LIMIT 3`) hoặc log snippet.

---

## Notes for executors

- **Order matters**: complete Counter first (simplest pipeline), then PPE
  (needs engine), then Attendance (needs face DB + employee mapping).
- **Wait times**: counter aggregator sweep is 60s default; PPE ticks within
  ~10s of person in frame; attendance dedup window is 60s (consecutive
  walk-pasts of the same person within 60s are dropped silently — by design).
- **Don't skip the degraded-path tests** in PPE and Attendance — the whole
  point of PRs 2/5 was surfacing degraded states, so a green happy path
  alone doesn't prove the audit/warning paths work.
- If any module FAILs, capture the evidence first (don't restart / re-deploy
  while the failure is visible) — the troubleshooting matrix in the
  per-module runbook needs that evidence to diagnose.
