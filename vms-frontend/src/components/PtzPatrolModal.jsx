import React, { useState, useEffect, useRef } from 'react';
import apiClient from '../api/apiClient';
import { Play, Square, Plus, Trash2, X, Clock } from 'lucide-react';

const PtzPatrolModal = ({ cameraId, onClose }) => {
  const [presets, setPresets] = useState([]);
  const [patrolPoints, setPatrolPoints] = useState([]);
  const [newPreset, setNewPreset] = useState('');
  const [newDwell, setNewDwell] = useState(10);
  const [loading, setLoading] = useState(false);
  const [activePatrolId, setActivePatrolId] = useState(null);
  const patrolIntervalRef = useRef(null);
  const patrolIndexRef = useRef(0);

  useEffect(() => {
    fetchData();
    }, [cameraId]);

  useEffect(() => () => {
    clearInterval(patrolIntervalRef.current);
  }, []);

  const fetchData = async () => {
    setLoading(true);
    const p = await apiClient.ptzGetPresets(cameraId);
    if (p.success) setPresets(p.data?.presets || []);
    setLoading(false);
  };

  const handleAddEntry = async () => {
    if (!newPreset) return;
    const entry = { preset_token: newPreset, dwell_time_sec: parseInt(newDwell) };
    setPatrolPoints([...patrolPoints, { ...entry, name: presets.find(pr => pr.token === newPreset)?.name || newPreset }]);
  };

  const handleStart = async () => {
    if (patrolPoints.length === 0) return;

    clearInterval(patrolIntervalRef.current);
    patrolIndexRef.current = 0;
    setActivePatrolId(1);

    const goToCurrentPreset = async () => {
      const point = patrolPoints[patrolIndexRef.current % patrolPoints.length];
      if (!point) return;
      await apiClient.ptzGoToPreset(cameraId, point.preset_token);
      clearInterval(patrolIntervalRef.current);
      patrolIntervalRef.current = setInterval(() => {
        patrolIndexRef.current = (patrolIndexRef.current + 1) % patrolPoints.length;
        goToCurrentPreset();
      }, Math.max(1, point.dwell_time_sec) * 1000);
    };

    await goToCurrentPreset();
  };

  const handleStop = async () => {
    clearInterval(patrolIntervalRef.current);
    setActivePatrolId(null);
  };

  return (
    <div className="modal-overlay" style={{ position:'fixed', inset:0, background:'rgba(0,0,0,0.8)', display:'flex', alignItems:'center', justifyContent:'center', zIndex:1100, backdropFilter:'blur(5px)' }}>
      <div className="glass" style={{ width:'500px', padding:'25px', borderRadius:'10px', border:'1px solid var(--border)' }}>
        <div style={{ display:'flex', justifyContent:'space-between', alignItems:'center', marginBottom:'20px' }}>
          <div style={{ fontFamily:'Rajdhani', fontWeight:700, fontSize:'18px', color:'var(--accent)' }}>PTZ PATROL CONFIGURATION</div>
          <X size={20} style={{ cursor:'pointer' }} onClick={onClose} />
        </div>

        <div className="config-section" style={{ marginBottom:'20px' }}>
          <div style={{ fontSize:'11px', color:'var(--text-dim)', marginBottom:'8px' }}>THÊM ĐIỂM TUẦN TRA</div>
          <div style={{ display:'flex', gap:'8px' }}>
             <select className="config-select" style={{ flex:2 }} value={newPreset} onChange={e => setNewPreset(e.target.value)}>
                <option value="">Chọn Preset...</option>
                {presets.map(p => <option key={p.token} value={p.token}>{p.name}</option>)}
             </select>
             <div style={{ flex:1, display:'flex', alignItems:'center', gap:'5px' }}>
                <Clock size={14} color="var(--text-dim)" />
                <input className="config-input" type="number" value={newDwell} onChange={e => setNewDwell(e.target.value)} style={{ width:'100%' }} />
             </div>
             <button className="grid-btn" onClick={handleAddEntry} disabled={!newPreset}><Plus size={14} /></button>
          </div>
        </div>

        <div style={{ maxHeight:'200px', overflowY:'auto', background:'rgba(0,0,0,0.2)', borderRadius:'5px', padding:'10px', marginBottom:'20px', border:'1px solid var(--border)' }}>
          {patrolPoints.length === 0 ? (
            <div style={{ textAlign:'center', color:'var(--text-dim)', fontSize:'12px', padding:'20px' }}>Chưa có điểm tuần tra nào.</div>
          ) : (
            patrolPoints.map((p, idx) => (
              <div key={idx} style={{ display:'flex', justifyContent:'space-between', alignItems:'center', padding:'8px 10px', borderBottom: idx < patrolPoints.length-1 ? '1px solid var(--border)' : 'none' }}>
                <div style={{ fontSize:'13px' }}>
                   <span style={{ color:'var(--accent)', fontWeight:700, marginRight:10 }}>#{idx+1}</span>
                   {p.name}
                </div>
                <div style={{ display:'flex', gap:15, alignItems:'center' }}>
                   <span style={{ fontSize:'11px', color:'var(--text-dim)' }}>{p.dwell_time_sec}s</span>
                   <Trash2 size={14} color="var(--danger)" style={{ cursor:'pointer' }} onClick={() => setPatrolPoints(prev => prev.filter((_, i) => i !== idx))} />
                </div>
              </div>
            ))
          )}
        </div>

        <div style={{ display:'flex', gap:'10px' }}>
          {activePatrolId ? (
            <button className="pb-btn" onClick={handleStop} style={{ flex:1, background:'rgba(255,48,96,0.2)', border:'1px solid var(--danger)', color:'var(--danger)' }}>
              <Square size={16} style={{ marginRight:8 }} /> DỪNG TUẦN TRA
            </button>
          ) : (
            <button className="pb-btn" onClick={handleStart} disabled={patrolPoints.length === 0} style={{ flex:1, background:'var(--accent)', color:'#000', fontWeight:700 }}>
              <Play size={16} style={{ marginRight:8 }} /> BẮT ĐẦU TUẦN TRA
            </button>
          )}
          <button className="grid-btn" onClick={onClose} style={{ flex:1 }}>ĐÓNG</button>
        </div>
      </div>
    </div>
  );
};

export default PtzPatrolModal;
