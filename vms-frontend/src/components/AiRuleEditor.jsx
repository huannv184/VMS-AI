import React, { useState, useRef, useEffect, useCallback } from 'react';
import { MousePointer2, Move, LassoSelect, Trash2, Save, X, Type, Settings2 } from 'lucide-react';

const AiRuleEditor = ({ cameraId, onSave, onClose }) => {
  const canvasRef = useRef(null);
  const [points, setPoints] = useState([]);
  const [mode, setMode] = useState('polygon'); // 'line' or 'polygon'
  const [snapshot, setSnapshot] = useState(null);
  const [isDrawing, setIsDrawing] = useState(false);

  // Load snapshot on mount
  useEffect(() => {
    const fetchSnapshot = async () => {
       try {
         const res = await fetch(`/api/cameras/${cameraId}/snapshot`, { method: 'POST' });
         if (res.ok) {
           const blob = await res.blob();
           setSnapshot(URL.createObjectURL(blob));
         }
       } catch (e) { console.error('Snapshot failed', e); }
    };
    fetchSnapshot();
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
    if (points.length < 2) return alert('Vui lòng vẽ ít nhất 2 điểm!');
    onSave({
      type: mode,
      points: points,
      camera_id: cameraId
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
         {!snapshot && <div className="ai-dot-spin"></div>}
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
