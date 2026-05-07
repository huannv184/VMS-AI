import React, { useState, useEffect, useMemo, useCallback } from 'react';
import { Download } from 'lucide-react';
import useVmsStore from '../store/useVmsStore';
import apiClient from '../api/apiClient';

const API_BASE = ''; // Vite proxy forward đến :8000
const normalizeTimestamp = (v) => (v > 9999999999 ? v : v * 1000);

const EVENT_TYPES = [
  { value: 'all', label: 'Tất cả loại' },
  { value: 'PERSON_DETECTED', label: 'Phát hiện người' },
  { value: 'FACE_RECOGNIZED', label: 'Nhận diện khuôn mặt' },
  { value: 'INTRUSION', label: 'Xâm nhập' },
  { value: 'LINE_CROSSING', label: 'Vượt ranh giới' },
  { value: 'LOITERING', label: 'Lảng vảng' },
  { value: 'CROWD_DETECTED', label: 'Đám đông' },
  { value: 'PPE_VIOLATION', label: 'Vi phạm PPE' },
  { value: 'FIRE_DETECTED', label: 'Phát hiện lửa' },
];

const AlarmsView = () => {
  const alarms = useVmsStore((state) => state.alarms);
  const setAlarms = useVmsStore((state) => state.setAlarms);
  const cameras = useVmsStore((state) => state.cameras);

  // Filters
  const [filterType, setFilterType] = useState('all');
  const [filterCamera, setFilterCamera] = useState('all');
  const [filterEventType, setFilterEventType] = useState('all');
  const [dateFilter, setDateFilter] = useState('');
  
  // UI State
  const [isLoading, setIsLoading] = useState(false);
  const [selectedAlarm, setSelectedAlarm] = useState(null);

  // Fetch initial data or when filter logic is applied
  const fetchAlarms = useCallback(async () => {
    setIsLoading(true);
    try {
      const params = { limit: 200 };
      if (dateFilter) {
        const d = new Date(dateFilter);
        params.start_time = Math.floor(d.getTime() / 1000);
        params.end_time = params.start_time + 86400;
      }
      if (filterCamera !== 'all') params.camera_id = filterCamera;
      if (filterEventType !== 'all') params.event_type = filterEventType;

      const res = await apiClient.getEvents(params);
      if (res.success) {
        const mapped = (res.data?.events || []).map((evt) => {
          let meta = {};
          try { meta = JSON.parse(evt.metadata_json || '{}'); } catch (_) {}
          const tsMs = evt.timestamp ? normalizeTimestamp(evt.timestamp) : null;
          return {
            id: evt.id,
            time: tsMs ? new Date(tsMs).toLocaleTimeString('vi-VN') : '',
            date: tsMs ? new Date(tsMs).toLocaleDateString('vi-VN') : '',
            rawTimestamp: evt.timestamp,
            type: evt.label || evt.event_type || 'AI EVENT',
            desc: evt.message || evt.description || '',
            camera: evt.camera_name || `CAM-${evt.camera_id || '?'}`,
            camera_id: evt.camera_id,
            severity: evt.severity ? evt.severity.toLowerCase() : 'low',
            status: evt.status || 'new',
            img: evt.snapshot_url ? `${API_BASE}${evt.snapshot_url}` : null,
            rule_name: meta.rule_name || null,
            confidence: meta.confidence != null ? Math.round(meta.confidence * 100) : null,
          };
        });

        // Sort newest first
        mapped.sort((a, b) => (b.rawTimestamp || 0) - (a.rawTimestamp || 0));
        setAlarms(mapped);
      }
    } catch (err) {
      console.error('AlarmsView: refresh error', err);
    } finally {
      setIsLoading(false);
    }
  }, [dateFilter, filterCamera, filterEventType, setAlarms]);

  useEffect(() => {
    fetchAlarms();
  }, [fetchAlarms]);

  const handleDelete = async (id) => {
    if (!window.confirm("Bạn có chắc chắn muốn xóa sự kiện cảnh báo này?")) return;
    try {
      await apiClient.deleteEvent(id);
      setSelectedAlarm(null);
      setAlarms(alarms.filter(a => a.id !== id));
    } catch (e) {
      alert("Lỗi khi xóa sự kiện!");
    }
  };

  const handleExport = async () => {
    try {
      const res = await apiClient.exportAnalyticsCsv({ type: 'events' });
      if (!res.success) throw new Error(res.error || 'Failed to export');
      apiClient.saveBlobResponse(res, `events_export_${new Date().getTime()}.csv`);
    } catch (e) {
      alert("Lỗi xuất file báo cáo!");
    }
  };

  // Filter application (local severity filter on top of server-side filters)
  const filteredAlarms = useMemo(() => {
    return alarms.filter(a => filterType === 'all' || a.severity === filterType);
  }, [alarms, filterType]);

  const severityColor = (sev) => {
    switch(sev) {
      case 'high': return 'var(--danger)';
      case 'medium': return 'var(--warn)';
      case 'low': return 'var(--accent)';
      default: return 'var(--text-dim)';
    }
  };

  const severityBg = (sev) => {
    switch(sev) {
      case 'high': return 'rgba(255,48,96,0.15)';
      case 'medium': return 'rgba(255,184,0,0.15)';
      case 'low': return 'rgba(0,200,245,0.15)';
      default: return 'rgba(255,255,255,0.05)';
    }
  };

  return (
    <div className="view-panel active" style={{display:'flex', flexDirection:'column', padding: 0, position:'relative'}}>
      
      {/* Filter Bar */}
      <div className="grid-toolbar" style={{ flexShrink: 0 }}>
        <div className={`grid-btn ${filterType === 'all' ? 'active' : ''}`} onClick={() => setFilterType('all')}>Tất cả ({alarms.length})</div>
        <div className={`grid-btn ${filterType === 'high' ? 'active' : ''}`} onClick={() => setFilterType('high')}>
          <span style={{color:'var(--danger)', marginRight:4}}>●</span>Khẩn cấp ({alarms.filter(a => a.severity === 'high').length})
        </div>
        <div className={`grid-btn ${filterType === 'medium' ? 'active' : ''}`} onClick={() => setFilterType('medium')}>
          <span style={{color:'var(--warn)', marginRight:4}}>●</span>Cảnh báo ({alarms.filter(a => a.severity === 'medium').length})
        </div>
        <div className={`grid-btn ${filterType === 'low' ? 'active' : ''}`} onClick={() => setFilterType('low')}>
          <span style={{color:'var(--accent)', marginRight:4}}>●</span>Thông tin ({alarms.filter(a => a.severity === 'low').length})
        </div>
        
        <div className="toolbar-sep"></div>

        {/* Camera filter */}
        <select
          value={filterCamera}
          onChange={e => setFilterCamera(e.target.value)}
          style={{background:'var(--bg)', border:'1px solid var(--border)', color:'var(--text)', padding:'4px 8px', borderRadius:'3px', fontFamily:"'Share Tech Mono', monospace", fontSize:'11px', outline:'none'}}
        >
          <option value="all">Tất cả camera</option>
          {cameras.map(c => (
            <option key={c.id} value={c.id}>{c.name || `CAM-${c.id}`}</option>
          ))}
        </select>

        {/* Event type filter */}
        <select
          value={filterEventType}
          onChange={e => setFilterEventType(e.target.value)}
          style={{background:'var(--bg)', border:'1px solid var(--border)', color:'var(--text)', padding:'4px 8px', borderRadius:'3px', fontFamily:"'Share Tech Mono', monospace", fontSize:'11px', outline:'none'}}
        >
          {EVENT_TYPES.map(t => (
            <option key={t.value} value={t.value}>{t.label}</option>
          ))}
        </select>

        <div className="toolbar-sep"></div>

        <input
          type="date"
          value={dateFilter}
          onChange={(e) => setDateFilter(e.target.value)}
          style={{background:'var(--bg)', border:'1px solid var(--border)', color:'var(--text)', padding:'4px 8px', borderRadius:'3px', fontFamily:"'Share Tech Mono', monospace", fontSize:'11px', outline:'none'}}
        />
        {(dateFilter || filterCamera !== 'all' || filterEventType !== 'all') && (
          <div className="grid-btn" onClick={() => { setDateFilter(''); setFilterCamera('all'); setFilterEventType('all'); }}>
            Xóa Lọc
          </div>
        )}
        
        <div className="toolbar-sep"></div>
        <div className="grid-btn" onClick={fetchAlarms} style={{pointerEvents: isLoading ? 'none' : 'auto', opacity: isLoading ? 0.5 : 1}}>
          {isLoading ? <div className="ai-dot-spin" style={{width:'12px', height:'12px', marginRight:'5px', verticalAlign:'middle'}}></div> : '🔄 ' } Làm mới
        </div>
        
        <div className="grid-btn" onClick={handleExport} style={{ borderLeft: '1px solid var(--border)', marginLeft: 'auto' }}>
          <Download size={14} style={{ marginRight: 6 }} /> Xuất báo cáo (CSV)
        </div>
      </div>

      {/* Events List */}
      <div style={{ flex: 1, overflowY: 'auto', padding: '12px' }}>
        {filteredAlarms.length > 0 ? (
          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(300px, 1fr))', gap: '10px' }}>
            {filteredAlarms.map((alarm, index) => (
              <div
                key={alarm.id || index}
                className={`alert-item ${alarm.severity || 'low'}`}
                style={{ cursor: 'pointer', margin: 0 }}
                onClick={() => setSelectedAlarm(alarm)}
              >
                {alarm.img ? (
                  <div style={{ width: 80, height: 80, borderRadius: 4, overflow: 'hidden', flexShrink: 0, border: '1px solid var(--border)' }}>
                    <img src={alarm.img} alt="" style={{ width: '100%', height: '100%', objectFit: 'cover' }} onError={(e) => { e.target.style.display = 'none'; }} />
                  </div>
                ) : (
                  <div style={{ width: 80, height: 80, borderRadius: 4, background: '#111', flexShrink: 0, border: '1px solid var(--border)', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
                     <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="var(--text-dim)"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
                  </div>
                )}
                
                <div className="alert-info" style={{ flex: 1, minWidth: 0, paddingLeft: '4px' }}>
                  <div className="alert-title" style={{ whiteSpace:'nowrap', overflow:'hidden', textOverflow:'ellipsis', fontSize:'14px' }}>{alarm.desc || alarm.type}</div>
                  <div className="alert-meta" style={{ marginTop: '5px' }}>{alarm.camera}</div>
                  <div className="alert-meta" style={{color: severityColor(alarm.severity)}}>{alarm.type}</div>
                  {alarm.rule_name && (
                    <div style={{fontSize:'9px', color:'var(--accent)', fontFamily:"'Share Tech Mono',monospace", marginTop:'2px', opacity:0.8}}>
                      RULE: {alarm.rule_name}
                    </div>
                  )}
                  {alarm.confidence != null && (
                    <div style={{fontSize:'9px', color:'var(--text-dim)', fontFamily:"'Share Tech Mono',monospace"}}>
                      CONF: {alarm.confidence}%
                    </div>
                  )}
                </div>
                
                <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'flex-end', justifyContent: 'space-between', padding: '2px 0' }}>
                  <div className="alert-time">{alarm.time}</div>
                  <span style={{
                    fontSize: 9,
                    padding: '3px 6px',
                    borderRadius: 2,
                    fontFamily: "'Share Tech Mono', monospace",
                    background: severityBg(alarm.severity),
                    color: severityColor(alarm.severity),
                    border: `1px solid ${severityColor(alarm.severity)}`,
                    textTransform: 'uppercase',
                    fontWeight: 700,
                    letterSpacing: '1px'
                  }}>{alarm.severity || 'info'}</span>
                </div>
              </div>
            ))}
          </div>
        ) : (
          <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', height: '100%', color: 'var(--text-dim)', gap: 15 }}>
            <svg width="60" height="60" viewBox="0 0 24 24" fill="none" stroke="var(--text-dim)" strokeWidth="1" style={{opacity: 0.3}}>
              <path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9"/><path d="M13.73 21a2 2 0 0 1-3.46 0"/>
            </svg>
            <div style={{ fontSize: 16, fontWeight: 600, letterSpacing: 1 }}>CHƯA CÓ TRỮ LƯỢNG SỰ KIỆN NÀO</div>
            <div style={{ fontSize: 12, maxWidth: 350, textAlign: 'center', lineHeight: 1.6 }}>
              Hệ thống đang chờ các sự kiện báo động AI từ backend. Chọn một khoảng thời gian khác hoặc kiểm tra lại kết nối.
            </div>
            <button className="grid-btn primary" onClick={fetchAlarms} style={{ marginTop: 10, padding: '8px 20px' }}>
              🔄 LÀM MỚI DỮ LIỆU
            </button>
          </div>
        )}
      </div>

      {/* EVENT DETAIL MODAL */}
      {selectedAlarm && (
        <div style={{
          position: 'absolute', inset: 0, zIndex: 100,
          background: 'rgba(0,10,20,0.85)', backdropFilter: 'blur(5px)',
          display: 'flex', alignItems: 'center', justifyContent: 'center'
        }}>
          <div style={{
            width: '600px', background: 'var(--bg-panel)', border: '1px solid var(--border)', borderRadius: '8px', 
            boxShadow: '0 10px 40px rgba(0,0,0,0.8)', display: 'flex', flexDirection: 'column', overflow: 'hidden'
          }}>
            {/* Modal Header */}
            <div style={{padding: '12px 15px', borderBottom: '1px solid var(--border)', display: 'flex', justifyContent: 'space-between', alignItems: 'center', background:'var(--bg)'}}>
              <div style={{fontFamily: "'Rajdhani', sans-serif", fontSize: '15px', fontWeight: 700, letterSpacing: '1px', display:'flex', alignItems:'center', gap:'8px'}}>
                <span style={{width:'8px', height:'8px', borderRadius:'50%', background: severityColor(selectedAlarm.severity)}}></span>
                CHI TIẾT MÃ LỖI: {String(selectedAlarm.id).substring(0,8).toUpperCase()}
              </div>
              <div style={{cursor: 'pointer', opacity: 0.7}} onClick={() => setSelectedAlarm(null)}>
                 <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><line x1="18" y1="6" x2="6" y2="18"></line><line x1="6" y1="6" x2="18" y2="18"></line></svg>
              </div>
            </div>

            {/* Modal Content */}
            <div style={{display: 'flex', flex: 1}}>
               {/* Image Side */}
               <div style={{flex: 1, background: '#000', borderRight: '1px solid var(--border)', display:'flex', alignItems:'center', justifyContent:'center'}}>
                 {selectedAlarm.img ? (
                   <img src={selectedAlarm.img} alt="Event Snapshot" style={{width: '100%', maxHeight: '350px', objectFit: 'contain'}} />
                 ) : (
                   <div style={{color: 'var(--text-dim)', fontSize:'12px'}}>Không có ảnh đính kèm</div>
                 )}
               </div>
               
               {/* Details Side */}
               <div style={{width: '250px', padding: '15px', display:'flex', flexDirection:'column', gap:'15px', background:'var(--bg-card)'}}>
                 <div>
                   <div style={{fontSize:'10px', color:'var(--text-dim)', letterSpacing:'1px', marginBottom:'4px'}}>LOẠI CẢNH BÁO</div>
                   <div style={{fontSize:'14px', color: severityColor(selectedAlarm.severity), fontWeight: 700, fontFamily:"'Rajdhani',sans-serif"}}>{selectedAlarm.type}</div>
                 </div>
                 
                 <div>
                   <div style={{fontSize:'10px', color:'var(--text-dim)', letterSpacing:'1px', marginBottom:'4px'}}>MÔ TẢ SỰ KIỆN</div>
                   <div style={{fontSize:'12px', color:'var(--text-primary)', lineHeight:1.4}}>{selectedAlarm.desc}</div>
                 </div>
                 
                 <div style={{display:'flex', justifyContent:'space-between'}}>
                   <div>
                     <div style={{fontSize:'10px', color:'var(--text-dim)', letterSpacing:'1px', marginBottom:'4px'}}>NGUỒN</div>
                     <div style={{fontSize:'12px', color:'var(--text-primary)', fontFamily:"'Share Tech Mono',monospace"}}>{selectedAlarm.camera}</div>
                   </div>
                   <div>
                     <div style={{fontSize:'10px', color:'var(--text-dim)', letterSpacing:'1px', marginBottom:'4px'}}>THỜI GIAN</div>
                     <div style={{fontSize:'12px', color:'var(--accent)', fontFamily:"'Share Tech Mono',monospace"}}>{selectedAlarm.date} {selectedAlarm.time}</div>
                   </div>
                 </div>

                 {selectedAlarm.rule_name && (
                   <div>
                     <div style={{fontSize:'10px', color:'var(--text-dim)', letterSpacing:'1px', marginBottom:'4px'}}>QUY TẮC KÍCH HOẠT</div>
                     <div style={{fontSize:'12px', color:'var(--accent)', fontFamily:"'Share Tech Mono',monospace"}}>{selectedAlarm.rule_name}</div>
                   </div>
                 )}

                 {selectedAlarm.confidence != null && (
                   <div>
                     <div style={{fontSize:'10px', color:'var(--text-dim)', letterSpacing:'1px', marginBottom:'4px'}}>ĐỘ TIN CẬY</div>
                     <div style={{fontSize:'12px', color:'var(--accent3)', fontFamily:"'Share Tech Mono',monospace"}}>{selectedAlarm.confidence}%</div>
                   </div>
                 )}
                 
                 <div style={{flex:1}}></div>
                 
                 {/* Actions */}
                 <div style={{display:'flex', flexDirection:'column', gap:'8px'}}>
                    <div className="grid-btn" style={{textAlign:'center', padding:'10px', background:'var(--bg-hover)', color:'var(--text-primary)'}} onClick={() => setSelectedAlarm(null)}>
                      Đóng
                    </div>
                    <div 
                      className="grid-btn" 
                      style={{textAlign:'center', padding:'10px', background:'rgba(255,48,96,0.1)', color:'var(--danger)', border:'1px solid rgba(255,48,96,0.3)', fontWeight:700}}
                      onClick={() => handleDelete(selectedAlarm.id)}
                    >
                      XÓA CẢNH BÁO NÀY
                    </div>
                 </div>
               </div>
            </div>
          </div>
        </div>
      )}

    </div>
  );
};

export default AlarmsView;
