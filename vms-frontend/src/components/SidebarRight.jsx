import React, { useState } from 'react';
import useVmsStore from '../store/useVmsStore';

const SidebarRight = ({ isVisible = true }) => {
  const [activePanel, setActivePanel] = useState('alerts');
  const counts = useVmsStore((state) => state.counts);
  const alarms = useVmsStore((state) => state.alarms);
  const cameras = useVmsStore((state) => state.cameras);

  const onlineCams = cameras.filter(c => c.status === 'online').length;

  return (
    <div className="sidebar-right" style={{ display: isVisible ? 'flex' : 'none' }}>
      <div className="panel-tabs">
        <div className={`panel-tab ${activePanel === 'alerts' ? 'active' : ''}`} onClick={() => setActivePanel('alerts')}>Cảnh báo</div>
        <div className={`panel-tab ${activePanel === 'ai' ? 'active' : ''}`} onClick={() => setActivePanel('ai')}>AI</div>
        <div className={`panel-tab ${activePanel === 'stats' ? 'active' : ''}`} onClick={() => setActivePanel('stats')}>Thống kê</div>
      </div>

      <div className={`panel-content ${activePanel === 'alerts' ? 'active' : ''}`}>
        <div className="stat-grid">
          <div className="stat-card"><div className="stat-val red">{alarms?.filter(a => a.severity === 'high').length || 0}</div><div className="stat-label">Khẩn cấp</div></div>
          <div className="stat-card"><div className="stat-val orange">{alarms?.filter(a => a.severity === 'medium').length || 0}</div><div className="stat-label">Cảnh báo</div></div>
          <div className="stat-card"><div className="stat-val cyan">{alarms?.length || 0}</div><div className="stat-label">Hôm nay</div></div>
          <div className="stat-card"><div className="stat-val green">{onlineCams}</div><div className="stat-label">Cam live</div></div>
        </div>

        <div className="panel-section-title">Cảnh báo gần đây <span style={{color: 'var(--accent)', cursor: 'pointer'}}>Xem tất cả →</span></div>

        <div style={{ display: 'flex', flexDirection: 'column', gap: 10, overflowY: 'auto' }}>
          {alarms?.map(alarm => (
            <div key={alarm.id} className={`alert-item ${alarm.severity}`}>
              <div className="alert-icon">
                {alarm.type === 'FACE' || alarm.type === 'face' ? '👤' : alarm.type === 'PPE' || alarm.type === 'ppe' ? '🛡' : alarm.type === 'VEHICLE' || alarm.type === 'vehicle' ? '🚗' : '⚠'}
              </div>
              <div className="alert-info">
                <div className="alert-title">{alarm.desc || alarm.message || 'Sự kiện AI'}</div>
                <div className="alert-meta">{alarm.camera} · {alarm.type}</div>
                <div className="alert-time">{alarm.time}</div>
              </div>
              <div className="alert-ack" title="Xác nhận">✓</div>
            </div>
          ))}
          {(!alarms || alarms.length === 0) && (
            <div style={{ textAlign: 'center', padding: 40, color: 'var(--text-dim)', fontSize: 11 }}>
              CHƯA CÓ CẢNH BÁO MỚI
            </div>
          )}
        </div>
      </div>

      <div className={`panel-content ${activePanel === 'ai' ? 'active' : ''}`}>
        <div className="ai-analysis">
          <div className="ai-analysis-header"><div className="ai-dot"></div>Phân tích thời gian thực</div>
          <div className="ai-row"><span className="ai-row-label">Người phát hiện</span><span className="ai-row-val cyan">{counts.people} người</span></div>
          <div className="ai-row"><span className="ai-row-label">Xe phát hiện</span><span className="ai-row-val cyan">{counts.vehicles} xe</span></div>
          <div className="ai-row"><span className="ai-row-label">Cảnh báo hệ thống</span><span className="ai-row-val red">{counts.alerts} sự cố</span></div>
          <div className="ai-row"><span className="ai-row-label">Vi phạm PPE</span><span className="ai-row-val orange">{counts.ppeViolations} phát hiện</span></div>
        </div>
        <div className="ai-analysis">
          <div className="ai-analysis-header"><div className="ai-dot"></div>Độ chính xác model</div>
          <div className="ai-row" style={{flexDirection:'column', alignItems:'flex-start', gap:'4px'}}>
            <div style={{display:'flex', justifyContent:'space-between', width:'100%'}}><span className="ai-row-label">Phát hiện người</span><span className="ai-row-val green">--</span></div>
            <div className="confidence-bar" style={{width:'100%'}}><div className="cbar"><div className="cbar-fill" style={{width:'0%', background:'var(--accent3)'}}></div></div></div>
          </div>
        </div>
      </div>

      <div className={`panel-content ${activePanel === 'stats' ? 'active' : ''}`}>
        <div style={{padding: '10px 12px', borderBottom: '1px solid var(--border)'}}>
           <div style={{fontSize: '9px', letterSpacing: '1.5px', fontFamily: "'Rajdhani',sans-serif", fontWeight: 700, color: 'var(--text-dim)', marginBottom: '8px'}}>AI PHÁT HIỆN HÔM NAY</div>
           <div style={{display: 'flex', flexDirection: 'column', gap: '6px'}}>
             <div>
               <div style={{display: 'flex', justifyContent: 'space-between', marginBottom: '2px'}}><span style={{fontSize: '10px', color: 'var(--text-secondary)'}}>Người</span><span style={{fontFamily: "'Share Tech Mono',monospace", fontSize: '10px', color: 'var(--accent3)'}}>{counts.people}</span></div>
               <div className="bar-track"><div className="bar-fill" style={{width: `${Math.min(counts.people / 10, 100)}%`, background: 'var(--accent3)'}}></div></div>
             </div>
             <div>
               <div style={{display: 'flex', justifyContent: 'space-between', marginBottom: '2px'}}><span style={{fontSize: '10px', color: 'var(--text-secondary)'}}>Xe</span><span style={{fontFamily: "'Share Tech Mono',monospace", fontSize: '10px', color: 'var(--accent)'}}>{counts.vehicles}</span></div>
               <div className="bar-track"><div className="bar-fill" style={{width: `${Math.min(counts.vehicles / 5, 100)}%`, background: 'var(--accent)'}}></div></div>
             </div>
             <div>
               <div style={{display: 'flex', justifyContent: 'space-between', marginBottom: '2px'}}><span style={{fontSize: '10px', color: 'var(--text-secondary)'}}>Cảnh báo</span><span style={{fontFamily: "'Share Tech Mono',monospace", fontSize: '10px', color: 'var(--danger)'}}>{counts.alerts}</span></div>
               <div className="bar-track"><div className="bar-fill" style={{width: `${Math.min(counts.alerts / 3, 100)}%`, background: 'var(--danger)'}}></div></div>
             </div>
           </div>
        </div>
      </div>
    </div>
  );
};

export default SidebarRight;
