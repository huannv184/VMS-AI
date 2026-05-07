import React, { useState, useEffect } from 'react';
import { LayoutGrid, PlayCircle, BarChart2, Bell, Settings, Map, ShieldAlert, Users, Footprints, Car, UserCheck, MonitorPlay, ScanSearch, MapPin, LogOut } from 'lucide-react';
import useVmsStore from '../store/useVmsStore';
import apiClient from '../api/apiClient';

const Topbar = ({ activeTab, setActiveTab }) => {
  const isConnected = useVmsStore((state) => state.isConnected);
  const cameras = useVmsStore((state) => state.cameras);
  const alarms = useVmsStore((state) => state.alarms);

  const [clock, setClock] = useState(new Date().toLocaleTimeString('vi-VN'));
  const [currentUser, setCurrentUser] = useState(null);
  const [showUserMenu, setShowUserMenu] = useState(false);

  useEffect(() => {
    const timer = setInterval(() => {
      setClock(new Date().toLocaleTimeString('vi-VN'));
    }, 1000);
    return () => clearInterval(timer);
  }, []);

  useEffect(() => {
    let cancelled = false;
    apiClient.getCurrentUser().then((res) => {
      if (cancelled) return;
      if (res?.success && res.data) {
        setCurrentUser(res.data?.user || res.data);
      }
    });
    return () => { cancelled = true; };
  }, []);

  const handleLogout = async () => {
    // BUG-C3 FIX: tell the backend to bump token_version (invalidates JWT +
    // cookie) BEFORE we drop the token locally. Order matters — the request
    // needs the auth header to identify the caller.
    try { await apiClient.logout(); } catch (_) {}
    apiClient.clearToken();
    window.dispatchEvent(new Event('vms_logout'));
  };

  const userInitials = (currentUser?.username || 'AD').slice(0, 2).toUpperCase();
  const userLabel = currentUser?.full_name || currentUser?.username || 'Admin';

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
        <div
          className="user-chip"
          onClick={() => setShowUserMenu((v) => !v)}
          style={{ cursor: 'pointer', position: 'relative' }}
          title={currentUser ? `${userLabel} (#${currentUser.id ?? '-'})` : 'Đăng nhập'}
        >
          <div className="user-avatar">{userInitials}</div>
          <span>{userLabel}</span>
          {showUserMenu && (
            <div
              style={{
                position: 'absolute', top: '100%', right: 0, marginTop: 6, zIndex: 50,
                background: 'var(--bg-panel)', border: '1px solid var(--border)', borderRadius: 4,
                minWidth: 140, boxShadow: '0 4px 16px rgba(0,0,0,0.6)'
              }}
              onClick={(e) => e.stopPropagation()}
            >
              <div
                style={{ padding: '8px 12px', fontSize: 12, color: 'var(--text)', cursor: 'pointer', display: 'flex', alignItems: 'center', gap: 8 }}
                onClick={() => { setShowUserMenu(false); setActiveTab('settings'); }}
              >
                <Settings size={12} /> Cài đặt
              </div>
              <div
                style={{ padding: '8px 12px', fontSize: 12, color: 'var(--danger)', cursor: 'pointer', display: 'flex', alignItems: 'center', gap: 8, borderTop: '1px solid var(--border)' }}
                onClick={() => { setShowUserMenu(false); handleLogout(); }}
              >
                <LogOut size={12} /> Đăng xuất
              </div>
            </div>
          )}
        </div>
      </div>
    </div>
  );
};

export default Topbar;
