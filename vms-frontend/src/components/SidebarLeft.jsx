import React, { useState } from 'react';
import useVmsStore from '../store/useVmsStore';

const SidebarLeft = ({ isVisible = true }) => {
  const cameras = useVmsStore((state) => state.cameras);
  const systemStats = useVmsStore((state) => state.systemStats);
  const [activeCam, setActiveCam] = useState(null);
  const [expandedSections, setExpandedSections] = useState({
    system: true,
  });

  const toggleSection = (sec) => {
    setExpandedSections(prev => ({ ...prev, [sec]: !prev[sec] }));
  };

  const cpuPercent = Math.round(systemStats.cpu_percent || 0);
  const ramPercent = Math.round(systemStats.ram_percent || 0);
  const diskPercent = Math.round(systemStats.disk_percent || 0);
  const uptimeHours = Math.round((systemStats.uptime_seconds || 0) / 3600);

  const getBarColor = (val) => val > 80 ? 'var(--danger)' : val > 60 ? 'var(--warn)' : 'var(--accent)';
  const getValClass = (val) => val > 80 ? 'critical' : val > 60 ? 'warn' : 'ok';

  return (
    <div className="sidebar-left" id="sidebarLeft" style={{ display: isVisible ? 'flex' : 'none' }}>
      <div style={{padding:'12px', borderBottom:'1px solid var(--border)'}}>
        <div style={{position:'relative'}}>
          <input className="search-input" type="text" placeholder="Tìm camera..." style={{width:'100%'}} />
          <svg style={{position:'absolute', right:'10px', top:'7px', color:'var(--text-dim)', width:'12px', height:'12px'}} viewBox="0 0 14 14" fill="currentColor">
            <path d="M12.7 11.3L9.8 8.4A5 5 0 108.4 9.8l2.9 2.9a1 1 0 001.4-1.4zM5 8a3 3 0 110-6 3 3 0 010 6z"/>
          </svg>
        </div>
      </div>

      <div style={{flex:1, overflowY:'auto'}}>
        {/* Hệ thống Camera */}
        <div className="sidebar-section">
          <div className="sidebar-section-title" onClick={() => toggleSection('system')}>
            <span>📹 Hệ thống Camera</span>
            <span className="section-count">{cameras.length}</span>
          </div>
          {expandedSections.system && (
            <>
              {cameras.map(cam => (
                <div 
                  key={cam.id}
                  className={`tree-item sub ${activeCam === cam.id ? 'active' : ''}`} 
                  onClick={() => setActiveCam(cam.id)}
                >
                  <svg className="cam-icon" viewBox="0 0 14 14" fill="currentColor">
                    <circle cx="7" cy="7" r="3"/>
                    <circle cx="7" cy="7" r="5.5" fill="none" stroke="currentColor" strokeWidth="1"/>
                  </svg>
                  <span className="cam-name">{cam.name}</span>
                  <span className={`cam-status ${cam.status || 'offline'}`}></span>
                  {cam.status === 'alert' && <span className="cam-badge">!</span>}
                </div>
              ))}
              {cameras.length === 0 && (
                <div style={{padding:'10px 20px', fontSize:'11px', color:'var(--text-dim)', textAlign:'center'}}>
                   Chưa có camera nào được cấu hình
                </div>
              )}
            </>
          )}
        </div>
      </div>

      <div className="sidebar-bottom">
        <div className="server-label">
          <span>Server Status</span>
          <span className="server-online-dot"></span>
        </div>
        <div className="stat-bar-row">
          <span className="stat-bar-label">CPU</span>
          <span className={`stat-bar-val ${getValClass(cpuPercent)}`}>{cpuPercent}%</span>
        </div>
        <div className="bar-track"><div className="bar-fill" style={{width:`${cpuPercent}%`, background: getBarColor(cpuPercent)}}></div></div>
        <div className="stat-bar-row">
          <span className="stat-bar-label">RAM</span>
          <span className={`stat-bar-val ${getValClass(ramPercent)}`}>{ramPercent}%</span>
        </div>
        <div className="bar-track"><div className="bar-fill" style={{width:`${ramPercent}%`, background: getBarColor(ramPercent)}}></div></div>
        <div className="stat-bar-row">
          <span className="stat-bar-label">Storage</span>
          <span className={`stat-bar-val ${getValClass(diskPercent)}`}>{diskPercent}%</span>
        </div>
        <div className="bar-track"><div className="bar-fill" style={{width:`${diskPercent}%`, background: getBarColor(diskPercent)}}></div></div>
        <div className="stat-bar-row" style={{marginTop:'4px'}}>
          <span className="stat-bar-label">Uptime</span>
          <span className="stat-bar-val ok">{uptimeHours}h</span>
        </div>
      </div>
    </div>
  );
};

export default SidebarLeft;
