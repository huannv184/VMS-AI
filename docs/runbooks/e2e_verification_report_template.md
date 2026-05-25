# E2E Verification Report

> Template for ops to fill in after running the three per-module readiness
> runbooks against a real deployment. Copy this file to
> `e2e_reports/<YYYY-MM-DD>_<site>.md` and replace the placeholders.

**Date**: YYYY-MM-DD
**Environment**: <site / env name>
**Operator**: <name>
**Build / Commit**: <git rev-parse --short HEAD>
**Backend URL**: <url>
**Frontend URL**: <url>

---

## 1. Counter

**Status**: PASS | FAIL

**Steps executed**:
- <short note — which camera, which line, direction of crossing>
- <short note — wait time before checking /summary>

**Evidence**:
- API: `GET /api/counter/status` → <one-line summary, e.g. "running=true, total_lines=1">
- API: `GET /api/counter/summary?camera_id=<id>` → <one-line summary, e.g. "total_today=2 (in=1, out=1)">
- UI screenshot: <path/link>
- Log snippet: <path/link>

**Observed result**:
- <what happened during the test>

**Expected vs actual**:
- Expected: <what the runbook says should happen>
- Actual: <what you saw>

**If FAIL**:
- Symptom: <which step failed>
- Suspected cause: <from the troubleshooting matrix>
- Next action: <what to try / who to escalate to>

---

## 2. PPE

**Status**: PASS | FAIL

**Steps executed**:
- <short note — which camera enabled, env vs ai_config path>
- <short note — helmet on/off sequence>

**Evidence**:
- API: `GET /api/analytics/ppe/status` → <one-line summary, e.g. "health=ok, last_tick 3s ago, 1/1 cam">
- API: `GET /api/analytics/ppe_compliance` → <one-line summary, e.g. "compliance_rate=0.62, samples=42">
- UI screenshot: <path/link — badge + violation list>
- Log snippet: <path/link — ai_worker engine load + PPE tick lines>

**Observed result**:
- <what happened during the test, including violation type emitted>

**Expected vs actual**:
- Expected: <from the runbook>
- Actual: <what you saw>

**If FAIL**:
- Symptom: <which step failed>
- Suspected cause: <from the troubleshooting matrix>
- Next action: <what to try / who to escalate to>

---

## 3. Attendance

**Status**: PASS | FAIL

**Steps executed**:
- <short note — which person enrolled, which camera role>
- <short note — happy path + degraded path A (unlinked) and/or B (no role)>

**Evidence**:
- API: `GET /api/attendance/health` → <one-line summary, e.g. "health=degraded, unlinked=2, fallback=0">
- DB / API attendance row: <one-line summary, e.g. "id=152, person_id=7, employee_id=3, source_rule=door_role">
- UI screenshot: <path/link — badge + warning banner if degraded path was tested>
- Log snippet: <path/link>

**Observed result**:
- <what happened during the test>

**Expected vs actual**:
- Expected: <from the runbook>
- Actual: <what you saw>

**If FAIL**:
- Symptom: <which step failed>
- Suspected cause: <from the troubleshooting matrix>
- Next action: <what to try / who to escalate to>

---

## Summary

- Counter: PASS | FAIL
- PPE: PASS | FAIL
- Attendance: PASS | FAIL

**Open issues** (anything not blocking but worth tracking):
- <issue 1>
- <issue 2>

**Recommendation**:
- [ ] Ready for deploy
- [ ] Ready with caveats (list them above)
- [ ] Not ready (blockers must be resolved + re-run before next attempt)

---

## Notes for the next reviewer

- Files referenced (screenshots, logs) should be checked into
  `e2e_reports/<YYYY-MM-DD>_<site>/` alongside this report so they're
  reproducible.
- If a module FAILed, the **next** report against the same site should
  include either (a) evidence the underlying issue was fixed in a
  commit/config change, or (b) explicit acknowledgement that the failure is
  accepted-risk for this deploy with sign-off.
- Cross-reference any failed item against the troubleshooting matrix in the
  corresponding per-module runbook — those are kept in sync with the
  controller code.
