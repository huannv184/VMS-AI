// ==============================================================
// CountingLineEditor — modal for drawing a single counting line on
// a camera snapshot. Two clicks define A & B endpoints.
// Coordinates persist as normalized 0..1 (PeopleCountTracker contract).
// ==============================================================
import React, { useState, useEffect, useRef, useCallback } from 'react';
import { Move, Save, Trash2, X } from 'lucide-react';
import apiClient from '../api/apiClient';

const OBJECT_CLASS_OPTIONS = [
  { value: 'person',     label: 'Người' },
  { value: 'car',        label: 'Ô tô' },
  { value: 'motorcycle', label: 'Xe máy' },
  { value: 'bicycle',    label: 'Xe đạp' },
  { value: 'truck',      label: 'Xe tải' },
  { value: 'bus',        label: 'Xe buýt' },
];

const CountingLineEditor = ({ cameraId, cameraName = '', initialLine = null, onSave, onClose }) => {
  const canvasRef = useRef(null);
  // Endpoints stored as 0..100 (percent) — same convention as AiRuleEditor.
  // Convert to 0..1 only on save.
  const [points, setPoints] = useState(() => {
    if (initialLine) {
      return [
        { x: initialLine.ax * 100, y: initialLine.ay * 100 },
        { x: initialLine.bx * 100, y: initialLine.by * 100 },
      ];
    }
    return [];
  });

  const [name,    setName]    = useState(initialLine?.name || '');
  const [labelA,  setLabelA]  = useState(initialLine?.direction_a_label || 'in');
  const [labelB,  setLabelB]  = useState(initialLine?.direction_b_label || 'out');
  const [enabled, setEnabled] = useState(initialLine?.enabled !== false);

  const [classes, setClasses] = useState(() => {
    try {
      const parsed = JSON.parse(initialLine?.object_classes_json || '["person"]');
      return Array.isArray(parsed) ? parsed : ['person'];
    } catch { return ['person']; }
  });

  const [snapshot, setSnapshot] = useState(null);
  const [snapshotError, setSnapshotError] = useState(null);
  const [saving, setSaving] = useState(false);

  // ── Snapshot loader (same pattern as AiRuleEditor) ─────────────────────
  useEffect(() => {
    let cancelled = false;
    let objectUrl = null;
    (async () => {
      try {
        const trig = await apiClient.snapshotCamera(cameraId);
        if (!trig.success) {
          if (!cancelled) setSnapshotError(trig.error || 'Không lấy được snapshot');
          return;
        }
        const url = trig.data?.snapshot_url || trig.data?.url;
        if (!url) { if (!cancelled) setSnapshotError('Backend không trả snapshot_url'); return; }
        const img = await apiClient.download(url);
        if (img.success && img.data instanceof Blob) {
          objectUrl = URL.createObjectURL(img.data);
          if (!cancelled) setSnapshot(objectUrl);
        } else if (!cancelled) {
          setSnapshotError(img.error || 'Lỗi tải ảnh');
        }
      } catch (e) {
        if (!cancelled) setSnapshotError(e.message || 'Lỗi snapshot');
      }
    })();
    return () => { cancelled = true; if (objectUrl) URL.revokeObjectURL(objectUrl); };
  }, [cameraId]);

  // ── Drawing ──────────────────────────────────────────────────────────────
  const redraw = useCallback(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    if (points.length === 0) return;

    const pxPoints = points.map(p => ({
      x: (p.x / 100) * canvas.width,
      y: (p.y / 100) * canvas.height,
    }));

    // The line itself
    if (pxPoints.length >= 2) {
      ctx.strokeStyle = enabled ? '#00c8f5' : '#888';
      ctx.lineWidth   = 4;
      ctx.beginPath();
      ctx.moveTo(pxPoints[0].x, pxPoints[0].y);
      ctx.lineTo(pxPoints[1].x, pxPoints[1].y);
      ctx.stroke();

      // Direction labels
      const midX = (pxPoints[0].x + pxPoints[1].x) / 2;
      const midY = (pxPoints[0].y + pxPoints[1].y) / 2;
      const dx = pxPoints[1].x - pxPoints[0].x;
      const dy = pxPoints[1].y - pxPoints[0].y;
      const len = Math.max(1, Math.sqrt(dx * dx + dy * dy));
      // Normal vector (perpendicular)
      const nx = -dy / len;
      const ny =  dx / len;
      ctx.font = 'bold 16px Rajdhani, sans-serif';
      ctx.fillStyle = '#32e827';
      ctx.fillText(`A · ${labelA}`, midX + nx * 30 - 30, midY + ny * 30);
      ctx.fillStyle = '#ff3060';
      ctx.fillText(`B · ${labelB}`, midX - nx * 30 - 30, midY - ny * 30);
    }

    // Endpoint handles
    pxPoints.forEach((p, i) => {
      ctx.fillStyle   = i === 0 ? '#32e827' : '#ff3060';
      ctx.strokeStyle = '#fff';
      ctx.lineWidth   = 2;
      ctx.beginPath();
      ctx.arc(p.x, p.y, 8, 0, Math.PI * 2);
      ctx.fill();
      ctx.stroke();
      ctx.fillStyle = '#fff';
      ctx.font = 'bold 12px Rajdhani';
      ctx.fillText(i === 0 ? 'A' : 'B', p.x - 4, p.y + 4);
    });
  }, [points, labelA, labelB, enabled]);

  useEffect(() => { redraw(); }, [redraw]);

  const handleCanvasClick = (e) => {
    if (points.length >= 2) return; // line is exactly 2 points
    const rect = canvasRef.current.getBoundingClientRect();
    const x = ((e.clientX - rect.left) / rect.width)  * 100;
    const y = ((e.clientY - rect.top)  / rect.height) * 100;
    setPoints((prev) => [...prev, { x, y }]);
  };

  const toggleClass = (cls) => {
    setClasses((prev) => prev.includes(cls)
      ? prev.filter((c) => c !== cls)
      : [...prev, cls]);
  };

  const handleSave = async () => {
    if (points.length !== 2) {
      alert('Vạch đếm cần đúng 2 điểm. Click trên ảnh để đặt A rồi B.');
      return;
    }
    if (classes.length === 0) {
      alert('Chọn ít nhất một class object để đếm.');
      return;
    }
    const payload = {
      camera_id:         cameraId,
      name:              name.trim() || `Line ${Date.now()}`,
      ax:                points[0].x / 100,
      ay:                points[0].y / 100,
      bx:                points[1].x / 100,
      by:                points[1].y / 100,
      direction_a_label: labelA.trim() || 'in',
      direction_b_label: labelB.trim() || 'out',
      object_classes:    classes,
      enabled,
    };
    setSaving(true);
    try {
      const res = initialLine
        ? await apiClient.updateCountingLine(initialLine.id, payload)
        : await apiClient.createCountingLine(payload);
      if (res.success) {
        onSave?.(payload);
      } else {
        alert(res.error || 'Lưu thất bại');
      }
    } catch (e) {
      alert(e.message || 'Lỗi mạng');
    } finally {
      setSaving(false);
    }
  };

  const inputStyle = {
    background: 'var(--bg)', border: '1px solid var(--border)',
    color: 'var(--text)', padding: '4px 8px', borderRadius: 3,
    fontFamily: "'Share Tech Mono', monospace", fontSize: 11, outline: 'none',
  };

  return (
    <div className="glass" style={{
      position: 'fixed', inset: '8%', zIndex: 1000,
      display: 'flex', flexDirection: 'column', borderRadius: 12,
      border: '1px solid var(--border)', overflow: 'hidden',
    }}>
      {/* Header */}
      <div style={{
        background: 'rgba(0,0,0,0.85)', padding: '10px 20px',
        display: 'flex', justifyContent: 'space-between', alignItems: 'center',
      }}>
        <div style={{ display: 'flex', gap: 12, alignItems: 'center' }}>
          <Move size={16} />
          <span style={{ fontSize: 14, fontWeight: 700, letterSpacing: 1 }}>
            COUNTING LINE EDITOR — {cameraName || `CAM-${cameraId}`}
          </span>
        </div>
        <X size={20} style={{ cursor: 'pointer' }} onClick={onClose} />
      </div>

      {/* Canvas + snapshot */}
      <div style={{
        flex: 1, position: 'relative', background: '#000',
        display: 'flex', alignItems: 'center', justifyContent: 'center',
      }}>
        {snapshot && <img src={snapshot} alt="" style={{ maxWidth: '100%', maxHeight: '100%', opacity: 0.6 }} />}
        <canvas
          ref={canvasRef} width={1280} height={720}
          onClick={handleCanvasClick}
          style={{ position: 'absolute', top: 0, left: 0, width: '100%', height: '100%', cursor: 'crosshair' }}
        />
        {!snapshot && snapshotError && (
          <div style={{ position: 'absolute', color: 'var(--danger)', fontSize: 12, textAlign: 'center', padding: 16 }}>
            ⚠ {snapshotError}<br />
            <span style={{ fontSize: 10, color: 'var(--text-dim)' }}>Vẫn có thể vẽ trên nền đen.</span>
          </div>
        )}
        {!snapshot && !snapshotError && <div className="ai-dot-spin" />}

        {/* On-canvas hint */}
        <div style={{
          position: 'absolute', bottom: 12, left: 12,
          background: 'rgba(0,0,0,0.7)', padding: '6px 10px', borderRadius: 4,
          fontSize: 11, color: '#fff',
        }}>
          {points.length === 0 && '👆 Click để đặt điểm A'}
          {points.length === 1 && '👆 Click tiếp để đặt điểm B'}
          {points.length === 2 && '✓ Đã đủ 2 điểm — kéo dài hoặc lưu'}
        </div>
      </div>

      {/* Footer form */}
      <div style={{ background: 'rgba(0,0,0,0.85)', padding: 14 }}>
        <div style={{ display: 'flex', gap: 10, flexWrap: 'wrap', alignItems: 'center', marginBottom: 10 }}>
          <label style={{ fontSize: 10, color: 'var(--text-secondary)' }}>Tên</label>
          <input style={{ ...inputStyle, width: 160 }}
                 value={name} onChange={(e) => setName(e.target.value)}
                 placeholder="Cửa chính" />

          <label style={{ fontSize: 10, color: 'var(--text-secondary)' }}>Label A→B</label>
          <input style={{ ...inputStyle, width: 70 }}
                 value={labelB} onChange={(e) => setLabelB(e.target.value)}
                 placeholder="out"
                 title="Label khi track đi từ A sang B (vd: out — đi ra)" />

          <label style={{ fontSize: 10, color: 'var(--text-secondary)' }}>Label B→A</label>
          <input style={{ ...inputStyle, width: 70 }}
                 value={labelA} onChange={(e) => setLabelA(e.target.value)}
                 placeholder="in"
                 title="Label khi track đi từ B sang A (vd: in — đi vào)" />

          <label style={{ fontSize: 10, color: 'var(--text-secondary)' }}>
            <input type="checkbox" checked={enabled} onChange={(e) => setEnabled(e.target.checked)} /> Kích hoạt
          </label>
        </div>

        <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap', alignItems: 'center' }}>
          <span style={{ fontSize: 10, color: 'var(--text-secondary)', marginRight: 4 }}>Đếm class:</span>
          {OBJECT_CLASS_OPTIONS.map((o) => (
            <button key={o.value}
              className={`grid-btn ${classes.includes(o.value) ? 'active' : ''}`}
              style={{ fontSize: 10, padding: '4px 8px' }}
              onClick={() => toggleClass(o.value)}>
              {o.label}
            </button>
          ))}
        </div>

        <div style={{
          display: 'flex', justifyContent: 'space-between', alignItems: 'center',
          marginTop: 12, paddingTop: 10, borderTop: '1px solid var(--border)',
        }}>
          <div style={{ fontSize: 10, color: 'var(--text-dim)' }}>
            * Người đi từ phía A (chấm xanh) sang B (chấm đỏ) → label "<b>{labelB || 'out'}</b>".<br />
            * Lưu xong, PeopleCountTracker tự reload — frame kế đã thấy line mới.
          </div>
          <div style={{ display: 'flex', gap: 8 }}>
            <button className="pb-btn"
              onClick={() => setPoints([])}
              style={{ background: 'transparent', border: '1px solid var(--danger)', color: 'var(--danger)' }}>
              <Trash2 size={14} style={{ marginRight: 5 }} /> XÓA ĐIỂM
            </button>
            <button className="pb-btn"
              onClick={handleSave} disabled={saving || points.length !== 2}
              style={{ background: 'var(--accent)', color: '#000', fontWeight: 'bold', opacity: points.length === 2 ? 1 : 0.5 }}>
              <Save size={14} style={{ marginRight: 5 }} /> {saving ? 'ĐANG LƯU...' : 'LƯU LINE'}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
};

export default CountingLineEditor;
