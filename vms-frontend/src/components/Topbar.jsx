import React, { useState, useEffect } from 'react';
import { LayoutGrid, PlayCircle, BarChart2, Bell, Settings, Map, ShieldAlert, Users, Footprints, Car, UserCheck, MonitorPlay, ScanSearch, MapPin } from 'lucide-react';
import useVmsStore from '../store/useVmsStore';

const Topbar = ({ activeTab, setActiveTab }) => {
  const isConnected = useVmsStore((state) => state.isConnected);
  const cameras = useVmsStore((state) => state.cameras);
  const alarms = useVmsStore((state) => state.alarms);

  const [clock, setClock] = useState(new Date().toLocaleTimeString('vi-VN'));

  useEffect(() => {
    const timer = setInterval(() => {
      setClock(new Date().toLocaleTimeString('vi-VN'));
    }, 1000);
    return () => clearInterval(timer);
  }, []);

  const onlineCams = cameras.filter(c => c.status === 'online').length;
  const totalCams = cameras.length;
  const highAlerts = alarms.filter(a => a.severity === 'high' && a.status === 'new').length;
  const totalNewAlarms = alarms.filter(a => a.status === 'new').length;

  return (
    <div className="topbar">
      <div className="logo">
        <svg viewBox="0 0 26 26" width="26" height="26" fill="none">
          <circle cx="13" cy="13" r="12" stroke="#00c8f5" strokeWidth="1.5" opacity="0.25"/>
          <circle cx="13" cy="13" r="7" stroke="#00c8f5" strokeWidth="1" opacity="0.5"/>
          <circle cx="13" cy="13" r="2.5" fill="#00c8f5"/>
          <path d="M13 1v3M13 22v3M1 13h3M22 13h3" stroke="#00c8f5" strokeWidth="1.5" opacity="0.4"/>
        </svg>
        <div>VMS-AI<span className="logo-sub">XPROTECT PROFESSIONAL</span></div>
      </div>
      <div className="topbar-divider"></div>
      <nav className="nav-tabs">
        <div className={`nav-tab ${activeTab === 'live' ? 'active' : ''}`} onClick={() => setActiveTab('live')}>
          <LayoutGrid size={13} style={{marginRight: 5}}/> Live
        </div>
        <div className={`nav-tab ${activeTab === 'playback' ? 'active' : ''}`} onClick={() => setActiveTab('playback')}>
          <PlayCircle size={13} style={{marginRight: 5}}/> Playback
        </div>
        <div className={`nav-tab ${activeTab === 'analytics' ? 'active' : ''}`} onClick={() => setActiveTab('analytics')}>
          <BarChart2 size={13} style={{marginRight: 5}}/> Analytics
        </div>
        <div className={`nav-tab ${activeTab === 'faceid' ? 'active' : ''}`} onClick={() => setActiveTab('faceid')}>
          <Users size={13} style={{marginRight: 5}}/> Face ID
        </div>
        <div className={`nav-tab ${activeTab === 'ppe' ? 'active' : ''}`} onClick={() => setActiveTab('ppe')}>
          <ShieldAlert size={13} style={{marginRight: 5}}/> PPE Monitor
        </div>
        <div className={`nav-tab ${activeTab === 'counter' ? 'active' : ''}`} onClick={() => setActiveTab('counter')}>
          <Footprints size={13} style={{marginRight: 5}}/> Counter
        </div>
        <div className={`nav-tab ${activeTab === 'attendance' ? 'active' : ''}`} onClick={() => setActiveTab('attendance')}>
          <UserCheck size={13} style={{marginRight: 5}}/> Attendance
        </div>
        <div className={`nav-tab ${activeTab === 'alarms' ? 'active' : ''}`} onClick={() => setActiveTab('alarms')}>
          <Bell size={13} style={{marginRight: 5}}/> Alarms
        </div>
        <div className={`nav-tab ${activeTab === 'floormap' ? 'active' : ''}`} onClick={() => setActiveTab('floormap')}>
          <Map size={13} style={{marginRight: 5}}/> Map
        </div>
        <div className={`nav-tab ${activeTab === 'sites' ? 'active' : ''}`} onClick={() => setActiveTab('sites')}>
          <MapPin size={13} style={{marginRight: 5}}/> Sites
        </div>
        <div className={`nav-tab ${activeTab === 'videowall' ? 'active' : ''}`} onClick={() => setActiveTab('videowall')}>
          <MonitorPlay size={13} style={{marginRight: 5}}/> Video Wall
        </div>
        <div className={`nav-tab ${activeTab === 'reid' ? 'active' : ''}`} onClick={() => setActiveTab('reid')}>
          <ScanSearch size={13} style={{marginRight: 5}}/> Re-ID
        </div>
      </nav>
      <div className="topbar-right">
        <div className={`status-pill ${isConnected ? 'live' : ''}`} style={{ borderColor: isConnected ? '' : 'var(--danger)', color: isConnected ? '' : 'var(--danger)', background: isConnected ? '' : 'rgba(255,48,96,0.1)' }}>
          <div className={`dot ${isConnected ? 'green' : 'red'}`} style={{ backgroundColor: isConnected ? '' : 'var(--danger)', boxShadow: isConnected ? '' : '0 0 6px var(--danger)' }}></div> 
          {isConnected ? 'HỆ THỐNG ONLINE' : 'MẤT KẾT NỐI'}
        </div>
        <div className={`status-pill ${onlineCams > 0 ? 'live' : ''}`}>
          <div className={`dot ${onlineCams > 0 ? 'green' : 'yellow'}`}></div> 
          LIVE · {onlineCams}/{totalCams} CAM
        </div>
        {highAlerts > 0 && (
          <div className="status-pill warn">
            <div className="dot yellow"></div> {highAlerts} ALERTS
          </div>
        )}
        <div className="clock" id="clock">{clock}</div>
        <div className={`icon-btn ${totalNewAlarms > 0 ? 'alert-active' : ''}`} title="Notifications">
          <Bell size={16} />
          {totalNewAlarms > 0 && <span className="badge">{totalNewAlarms > 99 ? '99+' : totalNewAlarms}</span>}
        </div>
        <div className="icon-btn" title="System Settings" style={{color: activeTab === 'settings' ? 'var(--accent)' : 'var(--text-secondary)'}} onClick={() => setActiveTab('settings')}>
          <Settings size={16} />
        </div>
        <div className="user-chip">
          <div className="user-avatar">AD</div>
          <span>Admin</span>
        </div>
      </div>
    </div>
  );
};

export default Topbar;
