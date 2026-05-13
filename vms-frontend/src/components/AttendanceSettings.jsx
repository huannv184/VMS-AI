import React, { useState, useEffect, useCallback } from 'react';
import { UserPlus, Trash2, Save, RefreshCw, Heart, Edit2, Clock, Plus, Calendar } from 'lucide-react';
import apiClient from '../api/apiClient';

const ROLE_OPTIONS = [
  { value: 'entry',   label: 'Cửa vào' },
  { value: 'exit',    label: 'Cửa ra' },
  { value: 'both',    label: 'Vào/Ra (toggle)' },
  { value: 'observe', label: 'Quan sát (không tính)' },
];

const EMPTY_SHIFT_DRAFT = {
  name: '', start_time_hm: '08:00', end_time_hm: '17:00',
  late_threshold_min: 15, grace_min: 0,
  ot_grace_min: 0, ot_min_minutes: 0, ot_max_minutes: 720,
  active: true,
};

// Strict HH:MM check, mirrors backend isValidHmTime so UI catches typos before
// the round-trip.
const isHm = (s) => typeof s === 'string' && /^([01]\d|2[0-3]):[0-5]\d$/.test(s);

const AttendanceSettings = ({ cameras = [], onToast = () => {} }) => {
  const [employees,   setEmployees]   = useState([]);
  const [shifts,      setShifts]      = useState([]);
  const [cameraRoles, setCameraRoles] = useState([]);
  const [health,      setHealth]      = useState({ pending_rows: 0, dropped_rows: 0 });
  const [loading,     setLoading]     = useState(false);

  const [editingEmp, setEditingEmp] = useState(null);
  const [draftEmp,   setDraftEmp]   = useState({ person_id: '', code: '', full_name: '', dept: '', shift_id: -1, active: true });

  const [editingShift, setEditingShift] = useState(null);
  const [draftShift,   setDraftShift]   = useState(EMPTY_SHIFT_DRAFT);

  // Holidays — minimal CRUD (list + add + delete). Edit-in-place punted; ops
  // can delete + re-add for the rare correction case.
  const todayIso = () => new Date().toISOString().slice(0, 10);
  const [holidays, setHolidays] = useState([]);
  const [draftHoliday, setDraftHoliday] = useState({ date: todayIso(), name: '', description: '' });
  const [holidayYear, setHolidayYear] = useState(new Date().getFullYear());

  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      // include_inactive=true so admins can see and re-activate disabled shifts.
      const [emp, sh, roles, hp, hd] = await Promise.all([
        apiClient.getAttendanceEmployees(),
        apiClient.getShifts(true),
        apiClient.getCameraRoles(),
        apiClient.getAttendanceHealth(),
        apiClient.getHolidays(holidayYear),
      ]);
      if (emp.success)   setEmployees(emp.data?.employees || []);
      if (sh.success)    setShifts(sh.data?.shifts || []);
      if (roles.success) setCameraRoles(roles.data?.camera_roles || []);
      if (hp.success)    setHealth(hp.data || { pending_rows: 0, dropped_rows: 0 });
      if (hd.success)    setHolidays(hd.data?.holidays || []);
    } catch (e) {
      console.error('AttendanceSettings refresh failed', e);
    } finally {
      setLoading(false);
    }
  }, [holidayYear]);

  useEffect(() => { refresh(); }, [refresh]);

  // Poll health every 10s so the operator notices BulkWriter backlog without
  // hammering the API. Drop the interval if the component unmounts.
  useEffect(() => {
    const id = setInterval(async () => {
      try {
        const hp = await apiClient.getAttendanceHealth();
        if (hp.success) setHealth(hp.data || { pending_rows: 0, dropped_rows: 0 });
      } catch (_) { /* ignore */ }
    }, 10000);
    return () => clearInterval(id);
  }, []);

  // ── Camera roles ─────────────────────────────────────────────────────────
  const roleByCam = React.useMemo(() => {
    const m = {};
    for (const r of cameraRoles) m[r.camera_id] = r.role;
    return m;
  }, [cameraRoles]);

  const handleSetRole = async (camera_id, role) => {
    try {
      const res = await apiClient.setCameraRole(camera_id, role);
      if (res.success) {
        onToast(`Đã đặt camera ${camera_id} = ${role}`, 'success');
        await refresh();
      } else {
        onToast(res.error || 'Lưu thất bại', 'error');
      }
    } catch (e) {
      onToast(e.message || 'Lỗi mạng', 'error');
    }
  };

  // ── Employees ────────────────────────────────────────────────────────────
  const startEdit = (emp) => {
    setEditingEmp(emp.id);
    setDraftEmp({
      person_id: emp.person_id,
      code:      emp.code || '',
      full_name: emp.full_name || '',
      dept:      emp.dept || '',
      shift_id:  emp.shift_id ?? -1,
      active:    emp.active,
    });
  };

  const cancelEdit = () => {
    setEditingEmp(null);
    setDraftEmp({ person_id: '', code: '', full_name: '', dept: '', shift_id: -1, active: true });
  };

  const saveEmp = async () => {
    const pid = parseInt(draftEmp.person_id, 10);
    if (!pid || pid <= 0) {
      onToast('person_id phải là số dương (lấy từ Face DB)', 'error');
      return;
    }
    const payload = {
      person_id: pid,
      code:      draftEmp.code,
      full_name: draftEmp.full_name,
      dept:      draftEmp.dept,
      // Backend treats shift_id<=0 as "not assigned" → bound to NULL.
      shift_id:  Number.isInteger(draftEmp.shift_id) && draftEmp.shift_id > 0 ? draftEmp.shift_id : -1,
      active:    !!draftEmp.active,
    };
    try {
      const res = editingEmp === 'new'
        ? await apiClient.createAttendanceEmployee(payload)
        : await apiClient.updateAttendanceEmployee(editingEmp, payload);
      if (res.success) {
        onToast(editingEmp === 'new' ? 'Đã thêm nhân viên' : 'Đã cập nhật', 'success');
        cancelEdit();
        await refresh();
      } else {
        onToast(res.error || 'Lưu thất bại', 'error');
      }
    } catch (e) {
      onToast(e.message || 'Lỗi mạng', 'error');
    }
  };

  const deleteEmp = async (id) => {
    if (!window.confirm('Vô hiệu hóa nhân viên này? (soft delete — vẫn giữ lịch sử chấm công)')) return;
    try {
      const res = await apiClient.deleteAttendanceEmployee(id);
      if (res.success) {
        onToast('Đã vô hiệu hóa nhân viên', 'warn');
        await refresh();
      } else {
        onToast(res.error || 'Xóa thất bại', 'error');
      }
    } catch (e) {
      onToast(e.message || 'Lỗi mạng', 'error');
    }
  };

  // ── Shifts ───────────────────────────────────────────────────────────────
  const shiftById = React.useMemo(() => {
    const m = {};
    for (const s of shifts) m[s.id] = s;
    return m;
  }, [shifts]);

  const startEditShift = (sh) => {
    setEditingShift(sh.id);
    setDraftShift({
      name: sh.name || '',
      start_time_hm: sh.start_time_hm || '08:00',
      end_time_hm:   sh.end_time_hm   || '17:00',
      late_threshold_min: sh.late_threshold_min ?? 15,
      grace_min:          sh.grace_min ?? 0,
      ot_grace_min:       sh.ot_grace_min   ?? 0,
      ot_min_minutes:     sh.ot_min_minutes ?? 0,
      ot_max_minutes:     sh.ot_max_minutes ?? 720,
      active: sh.active,
    });
  };

  const cancelEditShift = () => {
    setEditingShift(null);
    setDraftShift(EMPTY_SHIFT_DRAFT);
  };

  const saveShift = async () => {
    if (!draftShift.name || draftShift.name.trim().length === 0) {
      onToast('Tên ca không được để trống', 'error'); return;
    }
    if (!isHm(draftShift.start_time_hm)) {
      onToast('Giờ bắt đầu phải dạng HH:MM (24h)', 'error'); return;
    }
    if (!isHm(draftShift.end_time_hm)) {
      onToast('Giờ kết thúc phải dạng HH:MM (24h)', 'error'); return;
    }
    const lt = parseInt(draftShift.late_threshold_min, 10);
    const gr = parseInt(draftShift.grace_min, 10);
    if (!Number.isInteger(lt) || lt < 0 || lt > 720) {
      onToast('Ngưỡng trễ nặng phải 0..720 phút', 'error'); return;
    }
    if (!Number.isInteger(gr) || gr < 0 || gr > 720) {
      onToast('Ân hạn phải 0..720 phút', 'error'); return;
    }
    const otG = parseInt(draftShift.ot_grace_min,   10);
    const otN = parseInt(draftShift.ot_min_minutes, 10);
    const otX = parseInt(draftShift.ot_max_minutes, 10);
    if (!Number.isInteger(otG) || otG < 0 || otG > 720) {
      onToast('OT ân hạn phải 0..720 phút', 'error'); return;
    }
    if (!Number.isInteger(otN) || otN < 0 || otN > 1440) {
      onToast('OT tối thiểu phải 0..1440 phút', 'error'); return;
    }
    if (!Number.isInteger(otX) || otX < 0 || otX > 1440) {
      onToast('OT tối đa phải 0..1440 phút', 'error'); return;
    }
    if (otX > 0 && otN > otX) {
      onToast('OT tối thiểu không được lớn hơn OT tối đa', 'error'); return;
    }

    const payload = {
      name: draftShift.name.trim(),
      start_time_hm: draftShift.start_time_hm,
      end_time_hm:   draftShift.end_time_hm,
      late_threshold_min: lt,
      grace_min: gr,
      ot_grace_min:   otG,
      ot_min_minutes: otN,
      ot_max_minutes: otX,
      active: !!draftShift.active,
    };
    try {
      const res = editingShift === 'new'
        ? await apiClient.createShift(payload)
        : await apiClient.updateShift(editingShift, payload);
      if (res.success) {
        onToast(editingShift === 'new' ? 'Đã thêm ca' : 'Đã cập nhật ca', 'success');
        cancelEditShift();
        await refresh();
      } else {
        onToast(res.error || 'Lưu ca thất bại', 'error');
      }
    } catch (e) {
      onToast(e.message || 'Lỗi mạng', 'error');
    }
  };

  const deleteShift = async (id) => {
    if (!window.confirm('Vô hiệu hóa ca này? (soft delete — vẫn giữ lịch sử chấm công của nhân viên đã gán)')) return;
    try {
      const res = await apiClient.deleteShift(id);
      if (res.success) {
        onToast('Đã vô hiệu hóa ca', 'warn');
        await refresh();
      } else {
        onToast(res.error || 'Xóa ca thất bại', 'error');
      }
    } catch (e) {
      onToast(e.message || 'Lỗi mạng', 'error');
    }
  };

  const inputStyle = {
    background: 'var(--bg)', border: '1px solid var(--border)',
    color: 'var(--text)', padding: '4px 8px', borderRadius: 3,
    fontFamily: "'Share Tech Mono', monospace", fontSize: 11, outline: 'none',
  };
  const cellStyle = { padding: '6px 10px', fontSize: 11 };

  return (
    <div>
      {/* ── Health ──────────────────────────────────────────────────────── */}
      <div style={{
        display: 'flex', gap: 12, marginBottom: 16,
        padding: 10, background: 'var(--bg-card)',
        border: '1px solid var(--border)', borderRadius: 6,
      }}>
        <div style={{ flex: 1 }}>
          <div style={{ fontSize: 9, letterSpacing: 1, color: 'var(--text-secondary)' }}>HÀNG ĐỢI GHI DB</div>
          <div style={{ fontSize: 18, fontFamily: "'Share Tech Mono'", color: health.pending_rows > 1000 ? 'var(--warn)' : 'var(--accent3)' }}>
            {health.pending_rows}
          </div>
        </div>
        <div style={{ flex: 1 }}>
          <div style={{ fontSize: 9, letterSpacing: 1, color: 'var(--text-secondary)' }}>ROW BỊ DROP</div>
          <div style={{ fontSize: 18, fontFamily: "'Share Tech Mono'", color: health.dropped_rows > 0 ? 'var(--danger)' : 'var(--text-dim)' }}>
            {health.dropped_rows}
          </div>
        </div>
        <button className="config-btn" onClick={refresh} disabled={loading} style={{ alignSelf: 'center' }}>
          <RefreshCw size={12} /> {loading ? '...' : 'Làm mới'}
        </button>
      </div>

      {/* ── Camera roles ────────────────────────────────────────────────── */}
      <div className="config-section-title">Vai trò camera (entry / exit / both / observe)</div>
      <div style={{ marginBottom: 8, fontSize: 10, color: 'var(--text-secondary)' }}>
        Vào sự kiện FACE_RECOGNIZED, AttendanceTracker đọc role để quyết định kind.
        Chế độ <b>both</b> sẽ tự toggle in/out sau mỗi cooldown 60s.
      </div>
      <div style={{
        background: 'var(--bg-card)', border: '1px solid var(--border)',
        borderRadius: 6, padding: 8, marginBottom: 20,
      }}>
        <table style={{ width: '100%', borderCollapse: 'collapse' }}>
          <thead>
            <tr style={{ borderBottom: '1px solid var(--border)' }}>
              <th style={{ ...cellStyle, textAlign: 'left' }}>Camera</th>
              <th style={{ ...cellStyle, textAlign: 'left' }}>Role hiện tại</th>
              <th style={{ ...cellStyle, textAlign: 'left' }}>Đặt mới</th>
            </tr>
          </thead>
          <tbody>
            {cameras.length === 0 && (
              <tr><td colSpan="3" style={{ ...cellStyle, textAlign: 'center', color: 'var(--text-dim)' }}>
                Chưa có camera nào trong hệ thống.
              </td></tr>
            )}
            {cameras.map((cam) => (
              <tr key={cam.id} style={{ borderBottom: '1px solid var(--border)' }}>
                <td style={cellStyle}>
                  <span style={{ fontWeight: 700 }}>{cam.name || `CAM-${cam.id}`}</span>
                  <span style={{ marginLeft: 8, color: 'var(--text-dim)' }}>#{cam.id}</span>
                </td>
                <td style={cellStyle}>
                  <span style={{
                    color: roleByCam[cam.id] ? 'var(--accent3)' : 'var(--text-dim)',
                    fontFamily: "'Share Tech Mono'",
                  }}>
                    {roleByCam[cam.id] || '— chưa cấu hình —'}
                  </span>
                </td>
                <td style={cellStyle}>
                  <select
                    value={roleByCam[cam.id] || ''}
                    onChange={(e) => e.target.value && handleSetRole(cam.id, e.target.value)}
                    style={inputStyle}
                  >
                    <option value="">— chọn —</option>
                    {ROLE_OPTIONS.map((r) => (
                      <option key={r.value} value={r.value}>{r.label}</option>
                    ))}
                  </select>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {/* ── Shifts (work schedules) ─────────────────────────────────────── */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 6 }}>
        <div className="config-section-title" style={{ marginBottom: 0 }}>
          <Clock size={12} style={{ marginRight: 6, verticalAlign: '-2px' }} />
          Ca làm việc (lịch chấm công)
        </div>
        <button
          className="config-btn"
          onClick={() => { setEditingShift('new'); setDraftShift(EMPTY_SHIFT_DRAFT); }}
          style={{ padding: '4px 10px' }}
        >
          <Plus size={12} /> Thêm ca
        </button>
      </div>
      <div style={{ marginBottom: 8, fontSize: 10, color: 'var(--text-secondary)' }}>
        <b>Giờ bắt đầu</b> + <b>Ân hạn</b> = mốc chuẩn để đánh giá đi muộn. Sau ngưỡng đó tính
        từng phút trễ; nếu vượt <b>Ngưỡng trễ nặng</b> sẽ bị flag đỏ trong báo cáo.
        Xóa = vô hiệu hóa (soft) — vẫn giữ lịch sử chấm công cũ.
      </div>
      <div style={{
        background: 'var(--bg-card)', border: '1px solid var(--border)',
        borderRadius: 6, padding: 8, marginBottom: 20,
      }}>
        <table style={{ width: '100%', borderCollapse: 'collapse' }}>
          <thead>
            <tr style={{ borderBottom: '1px solid var(--border)' }}>
              <th style={{ ...cellStyle, textAlign: 'left' }}>Tên ca</th>
              <th style={{ ...cellStyle, textAlign: 'center' }}>Bắt đầu</th>
              <th style={{ ...cellStyle, textAlign: 'center' }}>Kết thúc</th>
              <th style={{ ...cellStyle, textAlign: 'center' }}>Ân hạn (phút)</th>
              <th style={{ ...cellStyle, textAlign: 'center' }}>Trễ nặng (phút)</th>
              <th style={{ ...cellStyle, textAlign: 'center' }} title="Phút sau giờ kết thúc trước khi tính OT (0 = tính ngay)">OT ân hạn</th>
              <th style={{ ...cellStyle, textAlign: 'center' }} title="OT tối thiểu để được tính (0 = mọi OT đều tính)">OT min</th>
              <th style={{ ...cellStyle, textAlign: 'center' }} title="Cap OT để loại outlier (mặc định 720 = 12h)">OT max</th>
              <th style={{ ...cellStyle, textAlign: 'center' }}>Active</th>
              <th style={{ ...cellStyle, textAlign: 'right' }}>Hành động</th>
            </tr>
          </thead>
          <tbody>
            {editingShift === 'new' && (
              <tr style={{ background: 'rgba(0,200,245,0.05)' }}>
                <td style={cellStyle}>
                  <input style={{ ...inputStyle, width: 160 }}
                         value={draftShift.name}
                         onChange={(e) => setDraftShift({ ...draftShift, name: e.target.value })}
                         placeholder="Ca sáng" />
                </td>
                <td style={{ ...cellStyle, textAlign: 'center' }}>
                  <input type="time" style={{ ...inputStyle, width: 90 }}
                         value={draftShift.start_time_hm}
                         onChange={(e) => setDraftShift({ ...draftShift, start_time_hm: e.target.value })} />
                </td>
                <td style={{ ...cellStyle, textAlign: 'center' }}>
                  <input type="time" style={{ ...inputStyle, width: 90 }}
                         value={draftShift.end_time_hm}
                         onChange={(e) => setDraftShift({ ...draftShift, end_time_hm: e.target.value })} />
                </td>
                <td style={{ ...cellStyle, textAlign: 'center' }}>
                  <input type="number" min="0" max="720" style={{ ...inputStyle, width: 60 }}
                         value={draftShift.grace_min}
                         onChange={(e) => setDraftShift({ ...draftShift, grace_min: parseInt(e.target.value || '0', 10) })} />
                </td>
                <td style={{ ...cellStyle, textAlign: 'center' }}>
                  <input type="number" min="0" max="720" style={{ ...inputStyle, width: 60 }}
                         value={draftShift.late_threshold_min}
                         onChange={(e) => setDraftShift({ ...draftShift, late_threshold_min: parseInt(e.target.value || '0', 10) })} />
                </td>
                <td style={{ ...cellStyle, textAlign: 'center' }}>
                  <input type="number" min="0" max="720" style={{ ...inputStyle, width: 55 }}
                         value={draftShift.ot_grace_min}
                         onChange={(e) => setDraftShift({ ...draftShift, ot_grace_min: parseInt(e.target.value || '0', 10) })} />
                </td>
                <td style={{ ...cellStyle, textAlign: 'center' }}>
                  <input type="number" min="0" max="1440" style={{ ...inputStyle, width: 55 }}
                         value={draftShift.ot_min_minutes}
                         onChange={(e) => setDraftShift({ ...draftShift, ot_min_minutes: parseInt(e.target.value || '0', 10) })} />
                </td>
                <td style={{ ...cellStyle, textAlign: 'center' }}>
                  <input type="number" min="0" max="1440" style={{ ...inputStyle, width: 55 }}
                         value={draftShift.ot_max_minutes}
                         onChange={(e) => setDraftShift({ ...draftShift, ot_max_minutes: parseInt(e.target.value || '0', 10) })} />
                </td>
                <td style={{ ...cellStyle, textAlign: 'center' }}>
                  <input type="checkbox" checked={draftShift.active}
                         onChange={(e) => setDraftShift({ ...draftShift, active: e.target.checked })} />
                </td>
                <td style={{ ...cellStyle, textAlign: 'right' }}>
                  <button className="config-btn" onClick={saveShift} style={{ padding: '3px 8px', marginRight: 4 }}>
                    <Save size={11} /> Lưu
                  </button>
                  <button className="config-btn danger" onClick={cancelEditShift} style={{ padding: '3px 8px' }}>Hủy</button>
                </td>
              </tr>
            )}
            {shifts.length === 0 && editingShift !== 'new' && (
              <tr><td colSpan="10" style={{ ...cellStyle, textAlign: 'center', color: 'var(--text-dim)' }}>
                Chưa có ca làm việc nào. Bấm "Thêm ca" để bắt đầu.
              </td></tr>
            )}
            {shifts.map((sh) => (
              <tr key={sh.id} style={{
                borderBottom: '1px solid var(--border)',
                opacity: sh.active ? 1 : 0.5,
                background: editingShift === sh.id ? 'rgba(0,200,245,0.05)' : 'transparent',
              }}>
                {editingShift === sh.id ? (
                  <>
                    <td style={cellStyle}>
                      <input style={{ ...inputStyle, width: 160 }}
                             value={draftShift.name}
                             onChange={(e) => setDraftShift({ ...draftShift, name: e.target.value })} />
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>
                      <input type="time" style={{ ...inputStyle, width: 90 }}
                             value={draftShift.start_time_hm}
                             onChange={(e) => setDraftShift({ ...draftShift, start_time_hm: e.target.value })} />
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>
                      <input type="time" style={{ ...inputStyle, width: 90 }}
                             value={draftShift.end_time_hm}
                             onChange={(e) => setDraftShift({ ...draftShift, end_time_hm: e.target.value })} />
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>
                      <input type="number" min="0" max="720" style={{ ...inputStyle, width: 60 }}
                             value={draftShift.grace_min}
                             onChange={(e) => setDraftShift({ ...draftShift, grace_min: parseInt(e.target.value || '0', 10) })} />
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>
                      <input type="number" min="0" max="720" style={{ ...inputStyle, width: 60 }}
                             value={draftShift.late_threshold_min}
                             onChange={(e) => setDraftShift({ ...draftShift, late_threshold_min: parseInt(e.target.value || '0', 10) })} />
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>
                      <input type="number" min="0" max="720" style={{ ...inputStyle, width: 55 }}
                             value={draftShift.ot_grace_min}
                             onChange={(e) => setDraftShift({ ...draftShift, ot_grace_min: parseInt(e.target.value || '0', 10) })} />
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>
                      <input type="number" min="0" max="1440" style={{ ...inputStyle, width: 55 }}
                             value={draftShift.ot_min_minutes}
                             onChange={(e) => setDraftShift({ ...draftShift, ot_min_minutes: parseInt(e.target.value || '0', 10) })} />
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>
                      <input type="number" min="0" max="1440" style={{ ...inputStyle, width: 55 }}
                             value={draftShift.ot_max_minutes}
                             onChange={(e) => setDraftShift({ ...draftShift, ot_max_minutes: parseInt(e.target.value || '0', 10) })} />
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>
                      <input type="checkbox" checked={draftShift.active}
                             onChange={(e) => setDraftShift({ ...draftShift, active: e.target.checked })} />
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'right' }}>
                      <button className="config-btn" onClick={saveShift} style={{ padding: '3px 8px', marginRight: 4 }}>
                        <Save size={11} /> Lưu
                      </button>
                      <button className="config-btn danger" onClick={cancelEditShift} style={{ padding: '3px 8px' }}>Hủy</button>
                    </td>
                  </>
                ) : (
                  <>
                    <td style={{ ...cellStyle, fontWeight: 700 }}>{sh.name}</td>
                    <td style={{ ...cellStyle, textAlign: 'center', fontFamily: "'Share Tech Mono'" }}>{sh.start_time_hm}</td>
                    <td style={{ ...cellStyle, textAlign: 'center', fontFamily: "'Share Tech Mono'" }}>{sh.end_time_hm}</td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>{sh.grace_min}</td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>{sh.late_threshold_min}</td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>{sh.ot_grace_min ?? 0}</td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>{sh.ot_min_minutes ?? 0}</td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>{sh.ot_max_minutes ?? 720}</td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>
                      {sh.active ? '✓' : <span style={{ color: 'var(--text-dim)' }}>—</span>}
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'right' }}>
                      <button className="config-btn" onClick={() => startEditShift(sh)} style={{ padding: '3px 8px', marginRight: 4 }}>
                        <Edit2 size={11} /> Sửa
                      </button>
                      <button className="config-btn danger" onClick={() => deleteShift(sh.id)} style={{ padding: '3px 8px' }}>
                        <Trash2 size={11} />
                      </button>
                    </td>
                  </>
                )}
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {/* ── Holidays (lịch nghỉ — tắt tính trễ/OT) ──────────────────────── */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 6 }}>
        <div className="config-section-title" style={{ marginBottom: 0 }}>
          <Calendar size={12} style={{ marginRight: 6, verticalAlign: '-2px' }} />
          Lịch nghỉ
        </div>
        <select
          value={holidayYear}
          onChange={(e) => setHolidayYear(parseInt(e.target.value, 10))}
          style={{ ...inputStyle, width: 90 }}
        >
          {[-1, 0, 1, 2].map((delta) => {
            const y = new Date().getFullYear() + delta;
            return <option key={y} value={y}>{y}</option>;
          })}
        </select>
      </div>
      <div style={{ marginBottom: 8, fontSize: 10, color: 'var(--text-secondary)' }}>
        Ngày trong bảng này sẽ KHÔNG tính trễ / OT — báo cáo chấm công hiện
        nhãn "Ngày lễ" cho mọi nhân viên ngày hôm đó (có punch hay không).
        Ngày lễ tái diễn hàng năm (Tết Dương lịch, Quốc khánh, …) cần thêm
        lại cho từng năm.
      </div>
      <div style={{
        background: 'var(--bg-card)', border: '1px solid var(--border)',
        borderRadius: 6, padding: 8, marginBottom: 20,
      }}>
        <table style={{ width: '100%', borderCollapse: 'collapse' }}>
          <thead>
            <tr style={{ borderBottom: '1px solid var(--border)' }}>
              <th style={{ ...cellStyle, textAlign: 'center', width: 130 }}>Ngày</th>
              <th style={{ ...cellStyle, textAlign: 'left' }}>Tên dịp</th>
              <th style={{ ...cellStyle, textAlign: 'left' }}>Ghi chú</th>
              <th style={{ ...cellStyle, textAlign: 'right', width: 110 }}>Hành động</th>
            </tr>
          </thead>
          <tbody>
            <tr style={{ background: 'rgba(0,200,245,0.05)' }}>
              <td style={{ ...cellStyle, textAlign: 'center' }}>
                <input type="date" style={{ ...inputStyle, width: 130 }}
                       value={draftHoliday.date}
                       onChange={(e) => setDraftHoliday({ ...draftHoliday, date: e.target.value })} />
              </td>
              <td style={cellStyle}>
                <input style={{ ...inputStyle, width: '100%' }}
                       placeholder="Tết Dương lịch"
                       value={draftHoliday.name}
                       onChange={(e) => setDraftHoliday({ ...draftHoliday, name: e.target.value })} />
              </td>
              <td style={cellStyle}>
                <input style={{ ...inputStyle, width: '100%' }}
                       placeholder="(tùy chọn)"
                       value={draftHoliday.description}
                       onChange={(e) => setDraftHoliday({ ...draftHoliday, description: e.target.value })} />
              </td>
              <td style={{ ...cellStyle, textAlign: 'right' }}>
                <button className="config-btn" style={{ padding: '3px 8px' }}
                        onClick={async () => {
                          if (!draftHoliday.name.trim()) { onToast('Tên dịp không được để trống', 'error'); return; }
                          if (!/^\d{4}-\d{2}-\d{2}$/.test(draftHoliday.date)) { onToast('Ngày phải dạng YYYY-MM-DD', 'error'); return; }
                          const res = await apiClient.createHoliday(draftHoliday);
                          if (res?.success) {
                            onToast(`Đã thêm ngày lễ ${draftHoliday.date}`, 'success');
                            setDraftHoliday({ date: todayIso(), name: '', description: '' });
                            await refresh();
                          } else if (res?.status === 403) {
                            onToast('Cần quyền admin để thêm ngày lễ', 'error');
                          } else if (res?.status === 409) {
                            onToast('Ngày này đã có trong lịch lễ', 'error');
                          } else {
                            onToast(res?.error || 'Thêm thất bại', 'error');
                          }
                        }}>
                  <Plus size={11} /> Thêm
                </button>
              </td>
            </tr>
            {holidays.map((h) => (
              <tr key={h.id}>
                <td style={{ ...cellStyle, textAlign: 'center', fontFamily: "'Share Tech Mono',monospace" }}>{h.date}</td>
                <td style={cellStyle}>{h.name}</td>
                <td style={{ ...cellStyle, color: 'var(--text-secondary)' }}>{h.description || ''}</td>
                <td style={{ ...cellStyle, textAlign: 'right' }}>
                  <button className="config-btn danger" style={{ padding: '3px 8px' }}
                          onClick={async () => {
                            if (!window.confirm(`Xóa ngày lễ ${h.date} – ${h.name}?`)) return;
                            const res = await apiClient.deleteHoliday(h.id);
                            if (res?.success) {
                              onToast('Đã xóa', 'success');
                              await refresh();
                            } else if (res?.status === 403) {
                              onToast('Cần quyền admin để xóa ngày lễ', 'error');
                            } else {
                              onToast(res?.error || 'Xóa thất bại', 'error');
                            }
                          }}>
                    <Trash2 size={11} />
                  </button>
                </td>
              </tr>
            ))}
            {holidays.length === 0 && (
              <tr>
                <td colSpan={4} style={{ ...cellStyle, textAlign: 'center', color: 'var(--text-secondary)' }}>
                  Chưa có ngày lễ nào cho năm {holidayYear}.
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>

      {/* ── Employees ───────────────────────────────────────────────────── */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 6 }}>
        <div className="config-section-title" style={{ marginBottom: 0 }}>
          Liên kết Nhân viên ↔ Face Database
        </div>
        <button
          className="config-btn"
          onClick={() => { setEditingEmp('new'); setDraftEmp({ person_id: '', code: '', full_name: '', dept: '', shift_id: -1, active: true }); }}
          style={{ padding: '4px 10px' }}
        >
          <UserPlus size={12} /> Thêm
        </button>
      </div>
      <div style={{ marginBottom: 8, fontSize: 10, color: 'var(--text-secondary)' }}>
        Một <code>person_id</code> trong Face DB ↔ một mã nhân viên. Chưa map = vẫn ghi attendance nhưng <code>employee_id = NULL</code>.
      </div>
      <div style={{
        background: 'var(--bg-card)', border: '1px solid var(--border)',
        borderRadius: 6, padding: 8,
      }}>
        <table style={{ width: '100%', borderCollapse: 'collapse' }}>
          <thead>
            <tr style={{ borderBottom: '1px solid var(--border)' }}>
              <th style={{ ...cellStyle, textAlign: 'left' }}>person_id</th>
              <th style={{ ...cellStyle, textAlign: 'left' }}>Mã NV</th>
              <th style={{ ...cellStyle, textAlign: 'left' }}>Họ tên</th>
              <th style={{ ...cellStyle, textAlign: 'left' }}>Phòng ban</th>
              <th style={{ ...cellStyle, textAlign: 'left' }}>Ca</th>
              <th style={{ ...cellStyle, textAlign: 'center' }}>Active</th>
              <th style={{ ...cellStyle, textAlign: 'right' }}>Hành động</th>
            </tr>
          </thead>
          <tbody>
            {editingEmp === 'new' && (
              <tr style={{ background: 'rgba(0,200,245,0.05)' }}>
                <td style={cellStyle}>
                  <input style={{ ...inputStyle, width: 70 }} type="number"
                         value={draftEmp.person_id}
                         onChange={(e) => setDraftEmp({ ...draftEmp, person_id: e.target.value })}
                         placeholder="42" />
                </td>
                <td style={cellStyle}>
                  <input style={{ ...inputStyle, width: 100 }}
                         value={draftEmp.code}
                         onChange={(e) => setDraftEmp({ ...draftEmp, code: e.target.value })}
                         placeholder="EMP001" />
                </td>
                <td style={cellStyle}>
                  <input style={{ ...inputStyle, width: 180 }}
                         value={draftEmp.full_name}
                         onChange={(e) => setDraftEmp({ ...draftEmp, full_name: e.target.value })}
                         placeholder="Nguyễn Văn A" />
                </td>
                <td style={cellStyle}>
                  <input style={{ ...inputStyle, width: 130 }}
                         value={draftEmp.dept}
                         onChange={(e) => setDraftEmp({ ...draftEmp, dept: e.target.value })}
                         placeholder="Kỹ thuật" />
                </td>
                <td style={cellStyle}>
                  <select style={{ ...inputStyle, width: 140 }}
                          value={draftEmp.shift_id ?? -1}
                          onChange={(e) => setDraftEmp({ ...draftEmp, shift_id: parseInt(e.target.value, 10) })}>
                    <option value={-1}>— chưa gán —</option>
                    {shifts.filter(s => s.active).map(s => (
                      <option key={s.id} value={s.id}>{s.name} ({s.start_time_hm}–{s.end_time_hm})</option>
                    ))}
                  </select>
                </td>
                <td style={{ ...cellStyle, textAlign: 'center' }}>
                  <input type="checkbox" checked={draftEmp.active}
                         onChange={(e) => setDraftEmp({ ...draftEmp, active: e.target.checked })} />
                </td>
                <td style={{ ...cellStyle, textAlign: 'right' }}>
                  <button className="config-btn" onClick={saveEmp} style={{ padding: '3px 8px', marginRight: 4 }}>
                    <Save size={11} /> Lưu
                  </button>
                  <button className="config-btn danger" onClick={cancelEdit} style={{ padding: '3px 8px' }}>Hủy</button>
                </td>
              </tr>
            )}

            {employees.length === 0 && editingEmp !== 'new' && (
              <tr><td colSpan="7" style={{ ...cellStyle, textAlign: 'center', color: 'var(--text-dim)' }}>
                Chưa có nhân viên nào. Bấm "Thêm" để liên kết.
              </td></tr>
            )}

            {employees.map((emp) => (
              <tr key={emp.id} style={{
                borderBottom: '1px solid var(--border)',
                opacity: emp.active ? 1 : 0.5,
                background: editingEmp === emp.id ? 'rgba(0,200,245,0.05)' : 'transparent',
              }}>
                {editingEmp === emp.id ? (
                  <>
                    <td style={cellStyle}>
                      <input style={{ ...inputStyle, width: 70 }} type="number"
                             value={draftEmp.person_id}
                             onChange={(e) => setDraftEmp({ ...draftEmp, person_id: e.target.value })} disabled />
                    </td>
                    <td style={cellStyle}>
                      <input style={{ ...inputStyle, width: 100 }}
                             value={draftEmp.code}
                             onChange={(e) => setDraftEmp({ ...draftEmp, code: e.target.value })} />
                    </td>
                    <td style={cellStyle}>
                      <input style={{ ...inputStyle, width: 180 }}
                             value={draftEmp.full_name}
                             onChange={(e) => setDraftEmp({ ...draftEmp, full_name: e.target.value })} />
                    </td>
                    <td style={cellStyle}>
                      <input style={{ ...inputStyle, width: 130 }}
                             value={draftEmp.dept}
                             onChange={(e) => setDraftEmp({ ...draftEmp, dept: e.target.value })} />
                    </td>
                    <td style={cellStyle}>
                      <select style={{ ...inputStyle, width: 140 }}
                              value={draftEmp.shift_id ?? -1}
                              onChange={(e) => setDraftEmp({ ...draftEmp, shift_id: parseInt(e.target.value, 10) })}>
                        <option value={-1}>— chưa gán —</option>
                        {shifts.filter(s => s.active).map(s => (
                          <option key={s.id} value={s.id}>{s.name} ({s.start_time_hm}–{s.end_time_hm})</option>
                        ))}
                      </select>
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>
                      <input type="checkbox" checked={draftEmp.active}
                             onChange={(e) => setDraftEmp({ ...draftEmp, active: e.target.checked })} />
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'right' }}>
                      <button className="config-btn" onClick={saveEmp} style={{ padding: '3px 8px', marginRight: 4 }}>
                        <Save size={11} /> Lưu
                      </button>
                      <button className="config-btn danger" onClick={cancelEdit} style={{ padding: '3px 8px' }}>Hủy</button>
                    </td>
                  </>
                ) : (
                  <>
                    <td style={{ ...cellStyle, fontFamily: "'Share Tech Mono'", color: 'var(--accent3)' }}>{emp.person_id}</td>
                    <td style={{ ...cellStyle, fontWeight: 700 }}>{emp.code || '—'}</td>
                    <td style={cellStyle}>{emp.full_name || '—'}</td>
                    <td style={cellStyle}>{emp.dept || '—'}</td>
                    <td style={cellStyle}>
                      {emp.shift_id > 0 && shiftById[emp.shift_id] ? (
                        <span style={{ color: 'var(--accent3)', fontFamily: "'Share Tech Mono'" }}>
                          {shiftById[emp.shift_id].name}
                          <span style={{ color: 'var(--text-dim)', marginLeft: 4 }}>
                            ({shiftById[emp.shift_id].start_time_hm})
                          </span>
                        </span>
                      ) : <span style={{ color: 'var(--text-dim)' }}>—</span>}
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'center' }}>
                      {emp.active ? '✓' : <span style={{ color: 'var(--text-dim)' }}>—</span>}
                    </td>
                    <td style={{ ...cellStyle, textAlign: 'right' }}>
                      <button className="config-btn" onClick={() => startEdit(emp)} style={{ padding: '3px 8px', marginRight: 4 }}>
                        <Edit2 size={11} /> Sửa
                      </button>
                      <button className="config-btn danger" onClick={() => deleteEmp(emp.id)} style={{ padding: '3px 8px' }}>
                        <Trash2 size={11} />
                      </button>
                    </td>
                  </>
                )}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
};

export default AttendanceSettings;
