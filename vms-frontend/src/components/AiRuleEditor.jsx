import React, { useState, useRef, useEffect, useCallback } from 'react';
import { Move, LassoSelect, Trash2, Save, X } from 'lucide-react';
import apiClient from '../api/apiClient';

const AiRuleEditor = ({ cameraId, onSave, onClose, initialZone = null }) => {
  const canvasRef = useRef(null);
  // Seed from initialZone so "SỬA VÙNG" actually edits the existing polygon
  // instead of starting blank and silently creating a duplicate on save.
  const [points, setPoints] = useState(() =>
    Array.isArray(initialZone?.points) ? initialZone.points : []
  );
  const [mode, setMode] = useState(initialZone?.type === 'line' ? 'line' : 'polygon');
  const [snapshot, setSnapshot] = useState(null);
  const [snapshotError, setSnapshotError] = useState(null);

  // Load snapshot on mount via apiClient (carries auth headers/cookies).
  useEffect(() => {
    let cancelled = false;
    let objectUrl = null;
    const fetchSnapshot = async () => {
      try {
        // POST snapshot triggers backend to capture; backend persists and returns JSON with snapshot_url.
        const triggerRes = await apiClient.snapshotCamera(cameraId);
        if (!triggerRes.success) {
          if (!cancelled) setSnapshotError(triggerRes.error || 'Không lấy được snapshot');
          return;
        }
        const url = triggerRes.data?.snapshot_url || triggerRes.data?.url;
        if (url) {
          // Fetch the image binary so we can render it as a blob URL (auth via cookie/header).
          const imgRes = await apiClient.download(url);
          if (imgRes.success && imgRes.data instanceof Blob) {
            objectUrl = URL.createObjectURL(imgRes.data);
            if (!cancelled) setSnapshot(objectUrl);
          } else if (!cancelled) {
            setSnapshotError(imgRes.error || 'Lỗi tải ảnh snapshot');
          }
        }
      } catch (e) {
        console.error('Snapshot failed', e);
        if (!cancelled) setSnapshotError(e.message || 'Lỗi snapshot');
      }
    };
    fetchSnapshot();
    return () => {
      cancelled = true;
      if (objectUrl) URL.revokeObjectURL(objectUrl);
    };
  }, [cameraId]);

  const redraw = useCallback(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, canvas.width, canvas.height);

    if (points.length < 1) return;

    ctx.lineWidth = 3;
    ctx.strokeStyle = 'var(--accent)';
    ctx.fillStyle = 'rgba(0, 200, 245, 0.2)';

    ctx.beginPath();
    points.forEach((p, i) => {
      const px = (p.x / 100) * canvas.width;
      const py = (p.y / 100) * canvas.height;
      if (i === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
      
      // Draw point handle
      ctx.save();
      ctx.fillStyle = '#fff';
      ctx.fillRect(px - 3, py - 3, 6, 6);
      ctx.restore();
    });

    if (mode === 'polygon' && points.length > 2) {
      ctx.closePath();
      ctx.fill();
    }
    ctx.stroke();

  }, [points, mode]);

  useEffect(() => {
    redraw();
  }, [redraw]);

  const handleCanvasClick = (e) => {
    const rect = canvasRef.current.getBoundingClientRect();
    const x = ((e.clientX - rect.left) / rect.width) * 100;
    const y = ((e.clientY - rect.top) / rect.height) * 100;
    
    setPoints(prev => [...prev, { x, y }]);
  };

  const handleSave = () => {
    if (mode === 'line' && points.length < 2) return alert('Vạch ảo cần ít nhất 2 điểm!');
    if (mode === 'polygon' && points.length < 3) return alert('Vùng đa giác cần ít nhất 3 điểm!');
    onSave({
      zone_id: initialZone?.zone_id ?? null,
      type: mode,
      points: points,
      camera_id: cameraId,
    });
  };

  return (
    <div className="glass" style={{ position: 'fixed', inset: '10%', zIndex: 1000, display:'flex', flexDirection:'column', borderRadius:12, border:'1px solid var(--border)', overflow:'hidden' }}>
      <div className="cam-header" style={{ background: 'rgba(0,0,0,0.8)', padding: '10px 20px', display:'flex', justifyContent:'space-between', alignItems:'center' }}>
         <div style={{ display:'flex', gap:15, alignItems:'center' }}>
            <span style={{ fontSize:14, fontWeight:700, letterSpacing:1 }}>AI ZONE EDITOR</span>
            <div className="toolbar-sep2" />
            <button className={`grid-btn ${mode === 'polygon' ? 'active' : ''}`} onClick={() => { setMode('polygon'); setPoints([]); }}>
               <LassoSelect size={14} style={{marginRight:5}}/> VÙNG XÂM NHẬP
            </button>
            <button className={`grid-btn ${mode === 'line' ? 'active' : ''}`} onClick={() => { setMode('line'); setPoints([]); }}>
               <Move size={14} style={{marginRight:5}}/> VẠCH ẢO
            </button>
         </div>
         <X size={20} style={{cursor:'pointer'}} onClick={onClose} />
      </div>

      <div style={{ flex:1, position:'relative', background:'#000', display:'flex', alignItems:'center', justifyContent:'center', overflow:'hidden' }}>
         {snapshot && <img src={snapshot} alt="Preview" style={{ maxWidth:'100%', maxHeight:'100%', opacity:0.5 }} />}
         <canvas 
            ref={canvasRef} 
            width={1280} 
            height={720} 
            onClick={handleCanvasClick}
            style={{ position:'absolute', top:0, left:0, width:'100%', height:'100%', cursor:'crosshair' }} 
         />
         {!snapshot && !snapshotError && <div className="ai-dot-spin"></div>}
         {!snapshot && snapshotError && (
           <div style={{ position: 'absolute', color: 'var(--danger)', fontSize: 12, textAlign: 'center', padding: 16 }}>
             ⚠ {snapshotError}<br />
             <span style={{ fontSize: 10, color: 'var(--text-dim)' }}>Bạn vẫn có thể vẽ vùng trên nền lưới phía dưới.</span>
           </div>
         )}
      </div>

      <div className="cam-footer" style={{ background: 'rgba(0,0,0,0.8)', padding: '15px 20px', display:'flex', justifyContent:'space-between' }}>
         <div style={{ fontSize:10, color:'var(--text-dim)' }}>
            * Click trên bản đồ để tạo điểm. Với Vùng xâm nhập, điểm cuối sẽ tự nối về điểm đầu.
         </div>
         <div style={{ display:'flex', gap:10 }}>
            <button className="pb-btn" onClick={() => setPoints([])} style={{ background:'transparent', border:'1px solid var(--danger)', color:'var(--danger)' }}>
               <Trash2 size={14} style={{marginRight:5}}/> XÓA TẤT CẢ
            </button>
            <button className="pb-btn" onClick={handleSave} style={{ background:'var(--accent)', color:'#000', fontWeight:'bold' }}>
               <Save size={14} style={{marginRight:5}}/> LƯU ZONE
            </button>
         </div>
      </div>
    </div>
  );
};

export default AiRuleEditor;
