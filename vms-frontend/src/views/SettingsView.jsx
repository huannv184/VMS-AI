import React, { useState, useEffect, useCallback } from 'react';
import useVmsStore from '../store/useVmsStore';
import apiClient from '../api/apiClient';
import BatchImportModal from '../components/BatchImportModal';
import AiRuleEditor from '../components/AiRuleEditor';
import TwoFactorSetupModal from '../components/TwoFactorSetupModal';
import PtzPatrolModal from '../components/PtzPatrolModal';

const defaultSettings = {
  storage_directory: '/mnt/recordings',
  storage_retention: '14 ngày',
  storage_quality: '1080p · 4Mbps',
  storage_autoDelete: true,
  storage_cloudBackup: false,
  
  net_ip: '192.168.1.100',
  net_rtsp_port: '554',
  net_web_port: '8080',
  net_bandwidth: '200 Mbps',
  net_ssl: true,

  ai_yolo: true,
  ai_lpr: true,
  ai_face: true,
  ai_behavior: true,
  ai_confidence: '80%',
  ai_gpu: true,
  
  ppe_helmet: true,
  ppe_vest: true,
  ppe_glasses: true,
  ppe_mask: true,
  
  alert_email_enabled: true,
  alert_email_address: 'alert@vms.com',
  alert_sms_enabled: false,
  alert_sound: true,
  alert_webhook_url: 'https://hooks.example.com/...',
  
  api_enabled: true,
  api_rate_limit: '500 req/phút',
  
  backup_schedule: 'Hàng ngày 06:00',
  backup_nas: true,
  backup_cloud: true,
  
  sys_default_record_mode: 'Liên tục 24/7'
};

// ─── Toast Component ────────────────────────────────
const Toast = ({ message, type, onClose }) => {
  useEffect(() => {
    const t = setTimeout(onClose, 3000);
    return () => clearTimeout(t);
  }, [onClose]);
  const colors = { success: 'var(--accent3)', error: 'var(--danger)', info: 'var(--accent)', warn: 'var(--warn)' };
  const bgColors = { success: 'rgba(50,232,39,0.12)', error: 'rgba(255,48,96,0.12)', info: 'rgba(0,200,245,0.12)', warn: 'rgba(255,184,0,0.12)' };
  const icons = { success: '✓', error: '✕', info: 'ℹ', warn: '⚠' };
  return (
    <div style={{
      position:'fixed', top:'60px', right:'20px', zIndex:9999,
      background: bgColors[type] || bgColors.info,
      border: `1px solid ${colors[type] || colors.info}`,
      color: colors[type] || colors.info,
      padding:'10px 18px', borderRadius:'4px', fontSize:'12px',
      fontFamily:"'Rajdhani',sans-serif", fontWeight:600, letterSpacing:'0.5px',
      display:'flex', alignItems:'center', gap:'8px',
      boxShadow:'0 4px 20px rgba(0,0,0,0.5)',
      backdropFilter:'blur(10px)',
      animation:'toastIn 0.3s ease'
    }}>
      <span style={{fontSize:'14px'}}>{icons[type] || icons.info}</span>
      {message}
      <span onClick={onClose} style={{marginLeft:'10px', cursor:'pointer', opacity:0.6, fontSize:'14px'}}>✕</span>
    </div>
  );
};

const SettingsView = () => {
  const cameras = useVmsStore((state) => state.cameras);
  const setCameras = useVmsStore((state) => state.setCameras);
  const removeCamera = useVmsStore((state) => state.removeCamera);
  const updateCameraStore = useVmsStore((state) => state.updateCamera);
  const systemStats = useVmsStore((state) => state.systemStats);
  
  const [activeNav, setActiveNav] = useState('cfgCameras');
  const [importModalOpen, setImportModalOpen] = useState(false);
  const [auditLogs, setAuditLogs] = useState([]);
  const [users, setUsers] = useState([]);
  const [devices, setDevices] = useState([]);
  const [settings, setSettings] = useState(defaultSettings);

  const [isSaving, setIsSaving] = useState(false);
  const [isDirty, setIsDirty] = useState(false);
  const [toast, setToast] = useState(null);

  // ─── Rules & Zones State ──────────────────────────────
  const [rules, setRules] = useState([]);
  const [zones, setZones] = useState([]);
  const [editingCameraForZone, setEditingCameraForZone] = useState(null);
  const [show2FAModal, setShow2FAModal] = useState(false);
  const [patrolCameraId, setPatrolCameraId] = useState(null);
  const [loadingRules, setLoadingRules] = useState(false);

  // ─── Counter Zones State ─────────────────────
  const [counterZones, setCounterZones] = useState([
    { id: 1, name: 'Cổng chính', camera: 'CAM-01', type: 'bidirectional', in: 0, out: 0, enabled: true },
    { id: 2, name: 'Bãi xe B2', camera: 'CAM-02', type: 'vehicle', in: 0, out: 0, enabled: true },
  ]);

  // ─── Roles State ──────────────────────────────
  const [rolesData] = useState([
    { id: 1, name: 'Administrator', desc: 'Toàn quyền hệ thống', perms: ['cameras','settings','users','ai','backup','audit','api'], count: 1, color: 'var(--danger)' },
    { id: 2, name: 'Operator', desc: 'Xem camera, quản lý sự kiện', perms: ['cameras','playback','events'], count: 2, color: 'var(--accent)' },
    { id: 3, name: 'Viewer', desc: 'Chỉ xem Live & Playback', perms: ['cameras','playback'], count: 0, color: 'var(--text-dim)' },
    { id: 4, name: 'AI Analyst', desc: 'Quản lý AI, Analytics, PPE', perms: ['ai','analytics','ppe','events'], count: 1, color: 'var(--purple)' },
  ]);

  // ─── Schedule Grid State ──────────────────────
  const [scheduleGrid, setScheduleGrid] = useState(() => {
    const grid = {};
    const days = ['T2','T3','T4','T5','T6','T7','CN'];
    days.forEach(d => { grid[d] = new Array(24).fill('record'); });
    // weekends less recording
    grid['T7'] = grid['T7'].map((v,i) => i >= 22 || i < 6 ? 'off' : 'record');
    grid['CN'] = grid['CN'].map((v,i) => i >= 22 || i < 6 ? 'off' : 'motion');
    return grid;
  });

  const showToast = useCallback((message, type = 'info') => {
    setToast({ message, type, key: Date.now() });
  }, []);

  const refreshCameras = useCallback(async () => {
    const res = await apiClient.getCameras();
    if (res.success) setCameras(res.data?.cameras || []);
    return res;
  }, [setCameras]);

  const refreshDevices = useCallback(async () => {
    const res = await apiClient.getDevices();
    if (res.success) setDevices(res.data?.devices || []);
    return res;
  }, []);

  const refreshUsers = useCallback(async () => {
    const res = await apiClient.getUsers();
    if (res.success) setUsers(res.data?.users || []);
    return res;
  }, []);

  const refreshRules = useCallback(async () => {
    const res = await apiClient.getRules();
    if (res.success) setRules(res.data?.rules || []);
    return res;
  }, []);

  const refreshZones = useCallback(async () => {
    const res = await apiClient.getZones();
    if (res.success) setZones(res.data?.zones || []);
    return res;
  }, []);

  useEffect(() => {
    apiClient.getSettings().then(res => {
      if (res.success) {
        setSettings(prev => ({ ...prev, ...(res.data || {}) }));
      }
    });
  }, []);

  useEffect(() => {
    if (activeNav === 'cfgAudit') {
      apiClient.getAuditLogs({ limit: 50 }).then((res) => {
        if (res.success) setAuditLogs(res.data?.logs || []);
      });
    } else if (activeNav === 'cfgRules') {
      setLoadingRules(true);
      refreshRules().finally(() => setLoadingRules(false));
    } else if (activeNav === 'cfgZones') {
      refreshZones();
    } else if (activeNav === 'cfgDevices') {
      refreshDevices();
    } else if (activeNav === 'cfgUsers') {
      refreshUsers();
    }

  }, [activeNav, refreshDevices, refreshRules, refreshUsers, refreshZones]);

  const handleChange = (key, value) => {
    setSettings(prev => ({ ...prev, [key]: value }));
    setIsDirty(true);
  };

  const handleToggle = (key) => {
    setSettings(prev => ({ ...prev, [key]: !prev[key] }));
    setIsDirty(true);
  };

  const handleSaveAll = async () => {
    setIsSaving(true);
    try {
      const res = await apiClient.updateSettings(settings);
      if (res.success) {
        setIsDirty(false);
        showToast('Đã lưu cấu hình thành công!', 'success');
      } else {
        showToast(res.error || 'Lưu thất bại. Yêu cầu quyền quản trị viên.', 'error');
      }
    } catch (e) {
      showToast('Lỗi kết nối đến máy chủ.', 'error');
    } finally {
      setIsSaving(false);
    }
  };

  const navItems = [
    { group: 'Hệ thống' },
    { id: 'cfgCameras', label: 'Camera', icon: <svg viewBox="0 0 14 14" fill="currentColor"><circle cx="7" cy="7" r="3"/><circle cx="7" cy="7" r="5.5" fill="none" stroke="currentColor" strokeWidth="1"/></svg> },
    { id: 'cfgDevices', label: 'Đầu ghi NVR/DVR', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><rect x="2" y="2" width="10" height="10" rx="1"/><path d="M2 5h10M2 8h10M5 2v10"/></svg> },

    { id: 'cfgStorage', label: 'Lưu trữ', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><rect x="1" y="3" width="12" height="3" rx="0.5"/><rect x="1" y="8" width="12" height="3" rx="0.5"/><circle cx="11" cy="4.5" r="0.8" fill="currentColor"/><circle cx="11" cy="9.5" r="0.8" fill="currentColor"/></svg> },
    { id: 'cfgNetwork', label: 'Mạng', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><circle cx="7" cy="3" r="1.5"/><circle cx="3" cy="11" r="1.5"/><circle cx="11" cy="11" r="1.5"/><path d="M7 4.5v2l-3 3M7 4.5v2l3 3"/></svg> },
    { group: 'AI & Phân tích' },
    { id: 'cfgAI', label: 'AI Engine', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><path d="M7 2c0 0-4 2-4 5s4 5 4 5 4-2 4-5-4-5-4-5z"/><circle cx="7" cy="7" r="1.5"/></svg> },
    { id: 'cfgRules', label: 'Quy tắc', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><path d="M2 4h10M2 7h7M2 10h5"/></svg> },
    { group: 'AI & Giám sát' },
    { id: 'cfgPPE', label: 'PPE Giám sát', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><circle cx="7" cy="4" r="2"/><path d="M3 12c0-2 2-3 4-3s4 1 4 3"/><path d="M7 7l-2 5M7 7l2 5"/></svg> },
    { id: 'cfgCounter', label: 'Đếm người/xe', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><path d="M2 7h10M4 2v10M10 2v10"/></svg> },
    { id: 'cfgSchedule', label: 'Lịch ghi hình', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><rect x="1" y="2" width="12" height="10" rx="1"/><path d="M4 1v2M10 1v2M1 5h12M4 8h2M8 8h2"/></svg> },
    { id: 'cfgZones', label: 'Vùng giám sát', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><path d="M2 2h10v10H2z" strokeDasharray="2 2"/></svg> },
    { group: 'Bảo mật' },
    { id: 'cfgUsers', label: 'Người dùng', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><circle cx="7" cy="5" r="2.5"/><path d="M2 12c0-2.8 2.2-5 5-5s5 2.2 5 5"/></svg> },
    { id: 'cfgRoles', label: 'Phân quyền', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><rect x="2" y="5" width="10" height="7" rx="1"/><path d="M4 5V3a3 3 0 0 1 6 0v2"/></svg> },
    { id: 'cfgAlerts', label: 'Cảnh báo', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><path d="M7 1L1 12h12L7 1z"/><path d="M7 5v4M7 11h.01"/></svg> },
    { id: 'cfgAudit', label: 'Nhật ký kiểm toán', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><path d="M2 2h7M2 7h10M2 12h10M10 2h.01"/></svg> },
    { group: 'Tích hợp' },
    { id: 'cfgIntegration', label: 'Tích hợp hệ thống', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><circle cx="7" cy="7" r="3"/><path d="M2 7h2M10 7h2M7 2v2M7 10v2"/></svg> },
    { id: 'cfgAPI', label: 'API & Webhook', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><path d="M4 10L1 7l3-3M10 10l3-3-3-3M6 12L8 2"/></svg> },
    { group: 'Hệ thống' },
    { id: 'cfgBackup', label: 'Sao lưu & Phục hồi', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><path d="M7 2a5 5 0 0 0-4.5 7.5L1 11v-1.5M7 12a5 5 0 0 0 4.5-7.5L13 3v1.5M7 4v3l2 2"/></svg> },
    { id: 'cfgSystem', label: 'Tổng quan hệ thống', icon: <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><circle cx="7" cy="7" r="2"/><path d="M7 1v2M7 11v2M1 7h2M11 7h2"/></svg> }
  ];

  return (
    <div className="view-panel active" style={{position: 'relative'}}>
      {/* Global Save Button */}
      <div style={{position: 'absolute', top: '15px', right: '30px', zIndex: 10}}>
         <button 
           className="pb-btn" 
           onClick={handleSaveAll} 
           style={{
             background: isDirty ? 'var(--accent)' : 'var(--bg-panel)',
             color: isDirty ? '#000' : 'var(--text-dim)',
             fontWeight: 700,
             opacity: isSaving ? 0.7 : 1,
             pointerEvents: isSaving ? 'none' : 'auto',
             transition: 'all 0.3s',
             border: isDirty ? '1px solid var(--accent)' : '1px solid var(--border)',
             boxShadow: isDirty ? '0 0 15px rgba(0,200,245,0.4)' : 'none'
           }}
         >
           {isSaving ? <div className="ai-dot-spin" style={{marginRight:6, width:12, height:12, borderColor:'#000', borderTopColor:'transparent'}}></div> : null}
           {isSaving ? 'ĐANG LƯU...' : (isDirty ? '💾 LƯU THAY ĐỔI' : 'TẤT CẢ ĐÃ LƯU')}
         </button>
      </div>

      <div className="config-view">
        <div className="config-nav">
          {navItems.map((item, idx) => {
            if (item.group) {
              return <div key={idx} className="config-nav-group">{item.group}</div>;
            }
            return (
              <div 
                key={item.id} 
                className={`config-nav-item ${activeNav === item.id ? 'active' : ''}`}
                onClick={() => setActiveNav(item.id)}
              >
                {item.icon} {item.label}
              </div>
            );
          })}
        </div>
        
        <div className="config-content">
          
          {/* CAMERAS */}
          {activeNav === 'cfgCameras' && (
            <div>
               <div className="config-section-title">Quản lý Camera</div>
               <div style={{display:'flex', gap:'8px', marginBottom:'12px'}}>
                 <div className="config-btn" onClick={() => document.dispatchEvent(new CustomEvent('vms:openModal'))}>
                   + Thêm Camera
                 </div>
                  <div className="config-btn" onClick={() => document.dispatchEvent(new CustomEvent('vms:openModal', { detail: { autoScan: true } }))}><svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5" style={{width:'11px',height:'11px',marginRight:'4px'}}><circle cx="7" cy="7" r="5.5"/><path d="M7 3.5v3.5l2 2"/></svg>Tự động tìm (ONVIF)</div>
                   <div className="config-btn" style={{background:'rgba(255,255,255,0.05)', border:'1px solid rgba(255,255,255,0.08)'}} onClick={() => setImportModalOpen(true)}>
                     <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" style={{width:'11px',height:'11px',marginRight:'4px'}}><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/></svg>
                     Import hàng loạt
                   </div>
               </div>
               
               {cameras.length > 0 ? cameras.map(cam => (
                 <div key={cam.id} className="cam-card">
                   <div className="cam-card-thumb">
                     <div style={{position:'absolute', inset:0, background: cam.status === 'online' ? 'radial-gradient(ellipse at 50% 50%,#0a1520,#040608)' : '#050709', display:'flex', alignItems:'center', justifyContent:'center'}}>
                       {cam.status === 'online' ? (
                         <div style={{width:'20px', height:'12px', border:'1px solid rgba(0,200,245,0.3)', borderRadius:'1px', position:'relative'}}>
                           <div style={{position:'absolute', right:'-4px', top:'3px', width:'4px', height:'6px', background:'rgba(0,200,245,0.2)', border:'1px solid rgba(0,200,245,0.3)'}}></div>
                         </div>
                       ) : (
                         <div style={{width:'8px',height:'8px',borderRadius:'50%',background:'var(--danger)',opacity:0.4}}></div>
                       )}
                     </div>
                   </div>
                   <div className="cam-card-info">
                     <div className="cam-card-name" style={{display:'flex', justifyContent:'space-between', alignItems:'baseline'}}>
                        <span>{cam.name}</span>
                        {cam.status === 'online' && (
                          <div style={{display:'flex', gap:'8px', fontSize:'10px', opacity:0.6, color:'var(--primary)', fontWeight:600}}>
                            <span>{cam.bitrate_kbps ? `${(cam.bitrate_kbps/1024).toFixed(1)} Mbps` : '1.2 Mbps'}</span>
                            <span>CPU: {cam.cpu_usage_percent || 15}%</span>
                          </div>
                        )}
                      </div>
                     <div className="cam-card-meta">{cam.rtsp_url ? cam.rtsp_url.replace(/\/\/.*@/, '//***@') : 'N/A'} · {cam.resolution || '1920x1080'} · {cam.fps || 0}fps</div>
                   </div>
                   <span className={`config-badge ${cam.status === 'online' ? 'active' : cam.status === 'error' ? 'error' : 'inactive'}`}>
                     {cam.status === 'error' && <span style={{marginRight:4}}>⚠️</span>}
                     {cam.status === 'online' ? 'Online' : cam.status === 'error' ? 'Lỗi kết nối' : 'Offline'}
                   </span>
                   <div className="cam-card-actions">
                     <div className="cam-card-btn" onClick={async () => {
                        try {
                          if (cam.status !== 'online') {
                            const res = await apiClient.startCamera(cam.id);
                            if (res.success) {
                              await refreshCameras();
                              showToast(`Camera ${cam.name} đã khởi động`, 'success');
                            } else { showToast(res.error || 'Không thể khởi động', 'error'); }
                          } else {
                            const res = await apiClient.stopCamera(cam.id);
                            if (res.success) {
                              await refreshCameras();
                              showToast(`Camera ${cam.name} đã dừng`, 'info');
                            } else { showToast(res.error || 'Không thể dừng', 'error'); }
                          }
                        } catch(e) { showToast('Lỗi kết nối', 'error'); }
                      }} title={cam.status === 'online' ? 'Dừng cam' : 'Khởi động cam'}><svg viewBox="0 0 14 14" fill="currentColor"><path d="M3 2L11 7 3 12z"/></svg></div>
                     <div className="cam-card-btn" onClick={() => document.dispatchEvent(new CustomEvent('vms:openModal', { detail: { camera: cam } }))} title="Chỉnh sửa"><svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><path d="M10 2l2 2-7 7H3v-2l7-7z"/></svg></div>
                     <div className="cam-card-btn danger" onClick={async () => {
                        if (!window.confirm('Xóa camera khỏi hệ thống?')) return;
                        try {
                          const res = await apiClient.deleteCamera(cam.id);
                          if (res.success) {
                            removeCamera(cam.id);
                            showToast(`Đã xóa camera ${cam.name}`, 'success');
                          } else { showToast(res.error || 'Không thể xóa camera', 'error'); }
                        } catch(e) { showToast('Lỗi kết nối khi xóa', 'error'); }
                      }}><svg viewBox="0 0 14 14" fill="none" stroke="var(--danger)" strokeWidth="1.5"><path d="M3 3h8l-1 9H4zM2 3h10M5 3V2h4v1"/></svg></div>
                   </div>
                 </div>
               )) : (
                 <div style={{textAlign:'center', padding:'30px', color:'var(--text-dim)', fontSize:12}}>Chưa có camera nào. Bấm "+ Thêm Camera" để bắt đầu.</div>
               )}
            </div>
          )}

          {/* DEVICES (NVR/DVR) */}
          {activeNav === 'cfgDevices' && (
            <div className="animate-in">
              <div className="config-section-title">Quản lý Đầu ghi (NVR/DVR)</div>
              <div style={{display:'flex', gap:'8px', marginBottom:'12px'}}>
                <div className="config-btn" onClick={() => {
                  const name = window.prompt("Tên đầu ghi:");
                  const ip = window.prompt("IP/Host:");
                  if (name && ip) {
                    apiClient.addDevice({ name, ip_address: ip, port: 80, brand: 'generic' }).then(res => {
                      if (res.success) {
                        refreshDevices();
                        showToast("Đã thêm đầu ghi mới", "success");
                      } else {
                        showToast(res.error || "Không thể thêm đầu ghi", "error");
                      }
                    });
                  }
                }}>+ Thêm Đầu Ghi</div>
              </div>

              <div style={{display:'grid', gridTemplateColumns:'repeat(auto-fill, minmax(300px, 1fr))', gap:'12px'}}>
                {devices.length > 0 ? devices.map(dev => (
                  <div key={dev.id} className="cam-card" style={{flexDirection:'column', alignItems:'stretch', padding:'15px', height:'auto'}}>
                    <div style={{display:'flex', justifyContent:'space-between', marginBottom:'10px'}}>
                      <div>
                        <div style={{fontWeight:700, fontSize:'14px', color:'var(--accent)'}}>{dev.name}</div>
                        <div style={{fontSize:'11px', color:'var(--text-dim)', fontFamily:"'Share Tech Mono',monospace"}}>{dev.ip_address}:{dev.port}</div>
                      </div>
                      <span className={`config-badge ${dev.status === 'online' ? 'active' : 'inactive'}`}>
                        {(dev.status || 'Offline').toUpperCase()}
                      </span>
                    </div>
                    
                    <div style={{fontSize:'10px', color:'var(--text-secondary)', marginBottom:'15px', borderLeft:'2px solid var(--accent)', paddingLeft:'8px'}}>
                      Thương hiệu: {(dev.brand || '').toUpperCase()} · Kênh: {dev.channel_count || 0}
                    </div>

                    <div style={{display:'flex', gap:'6px'}}>
                      <button className="config-btn" style={{flex:1, fontSize:'10px'}} onClick={async () => {
                        showToast(`Đang đồng bộ ${dev.name}...`, 'info');
                        const res = await apiClient.syncDevice(dev.id);
                        if (res.success) {
                          showToast(`Đã đồng bộ ${res.data?.new_cameras || 0} camera mới!`, 'success');
                          await refreshCameras();
                        } else {
                          showToast(res.error || "Lỗi đồng bộ", "error");
                        }
                      }}>Đồng bộ Cam</button>
                      
                      <button className="config-btn danger" style={{padding:'5px 10px'}} onClick={async () => {
                        if (window.confirm("Xóa đầu ghi?")) {
                          const res = await apiClient.deleteDevice(dev.id);
                          if (res.success) {
                            setDevices(devices.filter(d => d.id !== dev.id));
                            showToast("Đã xóa đầu ghi", "info");
                          } else {
                            showToast(res.error || "Không thể xóa đầu ghi", "error");
                          }
                        }
                      }}>
                        <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5" style={{width:'12px'}}><path d="M3 3h8l-1 9H4zM2 3h10M5 3V2h4v1"/></svg>
                      </button>
                    </div>
                  </div>
                )) : (
                  <div style={{gridColumn:'1/-1', textAlign:'center', padding:'40px', color:'var(--text-dim)', background:'rgba(255,255,255,0.02)', borderRadius:'8px', border:'1px dashed var(--border)'}}>
                    Chưa có đầu ghi NVR/DVR nào được kết nối.
                  </div>
                )}
              </div>
            </div>
          )}


          {/* STORAGE */}
          {activeNav === 'cfgStorage' && (
            <div>
              <div className="config-section-title">Cấu hình lưu trữ</div>
              <div className="config-section">
                <div className="config-row">
                  <div><div className="config-label">Thư mục lưu trữ Video</div><div className="config-desc">Đường dẫn tuyệt đối hoặc Map Drive hệ thống</div></div>
                  <input className="config-input" value={settings.storage_directory} onChange={e => handleChange('storage_directory', e.target.value)} style={{width: '200px'}} />
                </div>
                <div className="config-row">
                  <div><div className="config-label">Dung lượng phân vùng ổ đĩa</div><div className="config-desc">{systemStats.disk_used_gb?.toFixed(1) || 0} GB đã dùng / Tổng {systemStats.disk_total_gb?.toFixed(1) || 0} GB</div></div>
                  <div className="config-val">{Math.round(systemStats.disk_percent || 0)}%</div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Tự động xóa Video cũ (Auto-Purge)</div><div className="config-desc">Xóa Video khi ổ cứng đầy hơn 90% để tránh nghẽn server</div></div>
                  <div className={`toggle ${settings.storage_autoDelete ? 'on' : ''}`} onClick={() => handleToggle('storage_autoDelete')}></div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Thời gian lưu video</div></div>
                  <select className="config-select" value={settings.storage_retention} onChange={e => handleChange('storage_retention', e.target.value)}>
                    <option value="7 ngày">7 ngày</option>
                    <option value="14 ngày">14 ngày</option>
                    <option value="30 ngày">30 ngày</option>
                    <option value="90 ngày">90 ngày</option>
                  </select>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Chất lượng ghi hình nén</div></div>
                  <select className="config-select" value={settings.storage_quality} onChange={e => handleChange('storage_quality', e.target.value)}>
                    <option value="720p · 1Mbps">720p · 1Mbps (Tiết kiệm)</option>
                    <option value="1080p · 4Mbps">1080p · 4Mbps (Tối ưu)</option>
                    <option value="4K · 16Mbps">4K · 16Mbps (Rõ nét cao)</option>
                  </select>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Backup lên Cloud</div><div className="config-desc">Yêu cầu cấu hình S3 Keys</div></div>
                  <div className={`toggle ${settings.storage_cloudBackup ? 'on' : ''}`} onClick={() => handleToggle('storage_cloudBackup')}></div>
                </div>
              </div>
            </div>
          )}

          {/* NETWORK */}
          {activeNav === 'cfgNetwork' && (
            <div>
              <div className="config-section-title">Cấu hình Mạng & Server</div>
              <div className="config-section">
                <div className="config-row">
                  <div><div className="config-label">Địa chỉ Server IP (Streaming)</div></div>
                  <input className="config-input" value={settings.net_ip} onChange={e => handleChange('net_ip', e.target.value)} />
                </div>
                <div className="config-row">
                  <div><div className="config-label">Port nội bộ RTSP</div></div>
                  <input className="config-input" value={settings.net_rtsp_port} onChange={e => handleChange('net_rtsp_port', e.target.value)} style={{width:'80px'}} />
                </div>
                <div className="config-row">
                  <div><div className="config-label">Port bảng điều khiển Web</div></div>
                  <input className="config-input" value={settings.net_web_port} onChange={e => handleChange('net_web_port', e.target.value)} style={{width:'80px'}} />
                </div>
                <div className="config-row">
                  <div><div className="config-label">Bandwidth tối đa giới hạn</div></div>
                  <select className="config-select" value={settings.net_bandwidth} onChange={e => handleChange('net_bandwidth', e.target.value)}>
                    <option value="100 Mbps">100 Mbps</option>
                    <option value="200 Mbps">200 Mbps</option>
                    <option value="500 Mbps">500 Mbps</option>
                  </select>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Mã hóa HTTPS/SSL qua Web</div></div>
                  <div className={`toggle ${settings.net_ssl ? 'on' : ''}`} onClick={() => handleToggle('net_ssl')}></div>
                </div>
              </div>
            </div>
          )}

          {/* AI ENGINE */}
          {activeNav === 'cfgAI' && (
            <div>
              <div className="config-section-title">Cấu hình Cốt lõi AI Engine</div>
              <div className="config-section">
                <div className="config-row">
                  <div><div className="config-label">VMS Core: Nhận diện người, vật thể</div><div className="config-desc">Thuật toán nền tảng YOLOv8</div></div>
                  <div className={`toggle ${settings.ai_yolo ? 'on' : ''}`} onClick={() => handleToggle('ai_yolo')}></div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Nhận diện biển số thông minh (ANPR)</div><div className="config-desc">OCR + CNN cho hệ biển số Việt Nam (Vuông/Chữ nhật)</div></div>
                  <div className={`toggle ${settings.ai_lpr ? 'on' : ''}`} onClick={() => handleToggle('ai_lpr')}></div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Trích xuất khuôn mặt (FaceID)</div><div className="config-desc">Trích xuất 512-dim Embedding (FaceNet)</div></div>
                  <div className={`toggle ${settings.ai_face ? 'on' : ''}`} onClick={() => handleToggle('ai_face')}></div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Phân tích hành vi (Hành động)</div><div className="config-desc">Đánh nhau, té ngã, xâm nhập</div></div>
                  <div className={`toggle ${settings.ai_behavior ? 'on' : ''}`} onClick={() => handleToggle('ai_behavior')}></div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Ngưỡng tin cậy tối thiểu (Accuracy Threshold)</div><div className="config-desc">Loại bỏ các dự đoán tỷ lệ thấp</div></div>
                  <select className="config-select" value={settings.ai_confidence} onChange={e => handleChange('ai_confidence', e.target.value)}>
                    <option value="50%">50% (Cao)</option>
                    <option value="70%">70% (Khá)</option>
                    <option value="80%">80% (Khắt khe)</option>
                    <option value="90%">90% (Tuyệt đối)</option>
                  </select>
                </div>
                <div className="config-row">
                  <div><div className="config-label">GPU Hardware Acceleration</div><div className="config-desc">Tự động cấu hình CUDA/TensorRT</div></div>
                  <div style={{display:'flex', gap:'10px', alignItems:'center'}}>
                    <span className="config-badge active">{systemStats.gpu_memory_total_mb > 0 ? 'NVIDIA Kích Hoạt' : 'Chỉ CPU'}</span>
                    <div className={`toggle ${settings.ai_gpu ? 'on' : ''}`} onClick={() => handleToggle('ai_gpu')}></div>
                  </div>
                </div>
              </div>
            </div>
          )}

          {/* AI RULES */}
          {activeNav === 'cfgRules' && (
            <div>
              <div className="config-section-title">Quản lý Quy tắc AI (Rule Engine)</div>
              <div style={{display:'flex', gap:'8px', marginBottom:'12px'}}>
                <div className="config-btn" onClick={() => {
                   const newRule = { name: 'Quy tắc mới', event_type: 'intrusion', enabled: true, severity: 'high', actions: [] };
                   apiClient.addRule(newRule).then(res => {
                     if (res.success) {
                        refreshRules();
                        showToast('Đã tạo quy tắc mới', 'success');
                     } else {
                        showToast(res.error || 'Không thể tạo quy tắc', 'error');
                     }
                   });
                }}>+ Tạo Quy tắc mới</div>
              </div>

              {loadingRules ? (
                <div className="ai-dot-spin" style={{margin:'40px auto'}}></div>
              ) : (
                <div style={{display:'grid', gridTemplateColumns:'repeat(auto-fill, minmax(320px, 1fr))', gap:'12px'}}>
                  {rules.map(rule => (
                    <div key={rule.rule_id} className="glass" style={{padding:'16px', borderRadius:'10px', border:'1px solid var(--border)'}}>
                       <div style={{display:'flex', justifyContent:'space-between', alignItems:'baseline', marginBottom:'10px'}}>
                          <div style={{fontFamily:'Rajdhani', fontWeight:700, fontSize:'15px', color:'var(--accent)'}}>{rule.name}</div>
                          <div className={`toggle ${rule.enabled ? 'on' : ''}`} onClick={() => {
                             apiClient.updateRule(rule.rule_id, { ...rule, enabled: !rule.enabled }).then((res) => {
                                if (res.success) refreshRules();
                             });
                          }}></div>
                       </div>
                       
                       <div className="config-desc" style={{marginBottom:'10px'}}>
                          Loại: <span style={{color:'var(--accent3)'}}>{rule.event_type}</span> · 
                          Nghiêm trọng: <span className={`alarm-sev ${rule.severity === 'high' ? 'critical' : 'medium'}`}>{rule.severity}</span>
                       </div>

                       <div style={{display:'flex', gap:'5px', marginTop:'15px'}}>
                          <button className="grid-btn" style={{flex:1}} onClick={() => setEditingCameraForZone(cameras[0]?.id || 1)}>
                             ⚙ CẤU HÌNH VÙNG
                          </button>
                          <button className="grid-btn" style={{flex:1, color:'var(--danger)', border:'1px solid var(--danger)'}} onClick={() => {
                             if(window.confirm('Xóa quy tắc này?')) {
                                apiClient.deleteRule(rule.rule_id).then((res) => {
                                  if (res.success) refreshRules();
                                });
                             }
                          }}>
                             XÓA
                          </button>
                       </div>
                    </div>
                  ))}
                  {rules.length === 0 && <div style={{gridColumn:'1/-1', textAlign:'center', color:'var(--text-dim)', padding:'40px'}}>Chưa có quy tắc nào.</div>}
                </div>
              )}
            </div>
          )}

          {/* PPE */}
          {activeNav === 'cfgPPE' && (
            <div>
               <div className="config-section-title">Cài đặt Cảnh báo Đồng phục (PPE)</div>
               <div className="config-section">
                <div className="config-row">
                  <div><div className="config-label">🪖 Mũ bảo hiểm (Hard Hat)</div><div className="config-desc">Cảnh báo nếu nhân sự không đội mũ cứng</div></div>
                  <div className={`toggle ${settings.ppe_helmet ? 'on' : ''}`} onClick={() => handleToggle('ppe_helmet')}></div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">🦺 Áo phản quang</div><div className="config-desc">Phân loại áo bảo hộ cam, vàng, xanh</div></div>
                  <div className={`toggle ${settings.ppe_vest ? 'on' : ''}`} onClick={() => handleToggle('ppe_vest')}></div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">🥽 Kính bảo vệ</div><div className="config-desc">Che chắn mắt hoặc mặt nạ hàn</div></div>
                  <div className={`toggle ${settings.ppe_glasses ? 'on' : ''}`} onClick={() => handleToggle('ppe_glasses')}></div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">😷 Khẩu trang/Mặt nạ</div><div className="config-desc">N95, y tế, chống độc</div></div>
                  <div className={`toggle ${settings.ppe_mask ? 'on' : ''}`} onClick={() => handleToggle('ppe_mask')}></div>
                </div>
               </div>
            </div>
          )}

          {/* USERS */}
          {activeNav === 'cfgUsers' && (
            <div>
              <div className="config-section-title">Quản lý Tài khoản (Nhân sự)</div>
              {users.length > 0 ? (
                <table className="lpr-table">
                  <thead>
                    <tr>
                      <th style={{textAlign:'left', paddingLeft:'10px'}}>Username</th>
                      <th style={{textAlign:'left'}}>Thời gian tạo</th>
                      <th style={{textAlign:'left'}}>Quyền hạn</th>
                      <th style={{textAlign:'center'}}>2FA</th>
                      <th style={{textAlign:'center'}}>Actions</th>
                    </tr>
                  </thead>
                  <tbody>
                    {users.map(u => (
                      <tr key={u.id}>
                        <td style={{padding:'12px 10px', fontWeight:'bold'}}>{u.username}</td>
                        <td style={{fontFamily:"'Share Tech Mono',monospace", color:'var(--text-dim)'}}>{u.created_at ? new Date(u.created_at * 1000).toLocaleString() : ''}</td>
                        <td>
                           <span className={`config-badge ${u.role_id === 1 ? 'active' : 'inactive'}`} style={{background: u.role_id===1 ? 'rgba(255,48,96,0.1)' : '', borderColor: u.role_id===1 ? 'var(--danger)' : '', color: u.role_id===1 ? 'var(--danger)' : ''}}>
                             {u.role_id === 1 ? 'Administrator' : 'Operator'}
                           </span>
                        </td>
                        <td style={{textAlign:'center'}}>
                           <span style={{ 
                              color: u.two_factor_enabled ? 'var(--accent3)' : 'var(--text-dim)', 
                              fontSize: '11px', fontWeight: 700 
                           }}>
                              {u.two_factor_enabled ? '🔒 Đã bật' : '🔓 Chưa bật'}
                           </span>
                        </td>
                        <td style={{textAlign:'center', display:'flex', gap:'5px', justifyContent:'center'}}>
                           {!u.two_factor_enabled && (
                              <div className="config-btn" style={{padding:'4px 8px', fontSize:'10px', background:'var(--accent)', color:'#000'}} onClick={() => setShow2FAModal(true)}>Bật 2FA</div>
                           )}
                           <div className="config-btn danger" style={{padding:'4px 8px'}} onClick={() => window.alert('Tính năng quản trị bị khóa do phân quyền API')}>Xóa</div>
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              ) : (
                <div style={{textAlign:'center', padding:'30px', color:'var(--text-dim)'}}>Đang tải...</div>
              )}
            </div>
          )}

          {/* ALERTS */}
          {activeNav === 'cfgAlerts' && (
            <div>
              <div className="config-section-title">Kênh Xử lý Báo động</div>
              <div className="config-section">
                <div className="config-row">
                  <div><div className="config-label">Gửi Email tự động</div><div className="config-desc">Kèm theo ảnh chụp Snapshot</div></div>
                  <div style={{display:'flex', gap:'10px', alignItems:'center'}}>
                    <input className="config-input" disabled={!settings.alert_email_enabled} value={settings.alert_email_address} onChange={e => handleChange('alert_email_address', e.target.value)} />
                    <div className={`toggle ${settings.alert_email_enabled ? 'on' : ''}`} onClick={() => handleToggle('alert_email_enabled')}></div>
                  </div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Tin nhắn SMS / Zalo OA</div><div className="config-desc">API Gateway tích hợp</div></div>
                  <div className={`toggle ${settings.alert_sms_enabled ? 'on' : ''}`} onClick={() => handleToggle('alert_sms_enabled')}></div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Còi Hú tại Tủ trung tâm (Loa Speaker)</div></div>
                  <div className={`toggle ${settings.alert_sound ? 'on' : ''}`} onClick={() => handleToggle('alert_sound')}></div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Webhook Endpoints</div><div className="config-desc">Đẩy data trực tiếp về hệ thống khác</div></div>
                  <input className="config-input" value={settings.alert_webhook_url} onChange={e => handleChange('alert_webhook_url', e.target.value)} style={{width:'200px'}}/>
                </div>
              </div>
            </div>
          )}

          {/* API */}
          {activeNav === 'cfgAPI' && (
            <div>
              <div className="config-section-title">Hệ Sinh Thái Mở (API)</div>
              <div className="config-section">
                <div className="config-row">
                  <div><div className="config-label">Bật Cổng REST API</div><div className="config-desc">Máy nội bộ: http://{settings.net_ip}:{settings.net_web_port}/api</div></div>
                  <div className={`toggle ${settings.api_enabled ? 'on' : ''}`} onClick={() => handleToggle('api_enabled')}></div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">Access Token / Sinh mã Key</div></div>
                  <div style={{display:'flex', gap:'5px'}}>
                    <input className="config-input" value="vms_sk_live_2026_xYz...Hk" disabled type="password" style={{width:'150px'}} />
                    <div className="config-btn" onClick={() => { navigator.clipboard.writeText('vms_sk_live_2026_xYzHk'); showToast('Đã copy API Key!', 'success'); }}>Copy</div>
                  </div>
                </div>
                <div className="config-row">
                  <div><div className="config-label">API Rate Limit (Chống DDoD)</div></div>
                  <select className="config-select" value={settings.api_rate_limit} onChange={e => handleChange('api_rate_limit', e.target.value)}>
                    <option value="100 req/phút">100 req/phút</option>
                    <option value="500 req/phút">500 req/phút</option>
                    <option value="Không giới hạn">Không giới hạn</option>
                  </select>
                </div>
              </div>
            </div>
          )}

          {/* SCHEDULE - Visual Weekly Grid */}
          {activeNav === 'cfgSchedule' && (() => {
            const days = ['T2','T3','T4','T5','T6','T7','CN'];
            const modes = { record: { color: 'rgba(0,200,245,0.5)', label: 'Ghi liên tục' }, motion: { color: 'rgba(255,184,0,0.5)', label: 'Theo chuyển động' }, off: { color: 'rgba(255,255,255,0.04)', label: 'Tắt' } };
            const cycleMode = (day, hour) => {
              const order = ['record','motion','off'];
              setScheduleGrid(prev => {
                const next = { ...prev, [day]: [...prev[day]] };
                const cur = next[day][hour];
                next[day][hour] = order[(order.indexOf(cur) + 1) % 3];
                return next;
              });
              setIsDirty(true);
            };
            return (
             <div>
               <div className="config-section-title">Lịch Trình Ghi Hình (Record Schedule)</div>
               <div className="config-section">
                 <div className="config-row">
                   <div><div className="config-label">Chế độ mặc định (Toàn cụm Camera)</div><div className="config-desc">Ảnh hưởng tới toàn bộ cấu hình ổ cứng</div></div>
                   <select className="config-select" value={settings.sys_default_record_mode} onChange={e => handleChange('sys_default_record_mode', e.target.value)}>
                     <option value="Liên tục 24/7">Ghi đè - Liên tục 24/7</option>
                     <option value="Theo chuyển động">Tiết kiệm - Theo chuyển động AI</option>
                     <option value="Theo sự kiện">Siêu tiết kiệm - Theo sự kiện</option>
                   </select>
                 </div>
               </div>

               {/* Legend */}
               <div style={{display:'flex', gap:'15px', marginBottom:'10px', fontSize:'10px'}}>
                 {Object.entries(modes).map(([k,v]) => (
                   <div key={k} style={{display:'flex', alignItems:'center', gap:'5px'}}>
                     <div style={{width:'12px', height:'12px', borderRadius:'2px', background:v.color, border:'1px solid rgba(255,255,255,0.1)'}}></div>
                     <span style={{color:'var(--text-secondary)'}}>{v.label}</span>
                   </div>
                 ))}
                 <span style={{color:'var(--text-dim)', marginLeft:'auto', fontSize:'9px'}}>Click ô để chuyển chế độ</span>
               </div>

               {/* Grid Header */}
               <div style={{display:'grid', gridTemplateColumns:'40px repeat(24, 1fr)', gap:'2px', marginBottom:'2px'}}>
                 <div></div>
                 {Array.from({length:24}).map((_,i) => (
                   <div key={i} style={{textAlign:'center', fontSize:'7px', fontFamily:"'Share Tech Mono',monospace", color:'var(--text-dim)'}}>{String(i).padStart(2,'0')}</div>
                 ))}
               </div>

               {/* Grid Body */}
               {days.map(day => (
                 <div key={day} style={{display:'grid', gridTemplateColumns:'40px repeat(24, 1fr)', gap:'2px', marginBottom:'2px'}}>
                   <div style={{fontSize:'10px', fontFamily:"'Rajdhani',sans-serif", fontWeight:700, color:'var(--text-secondary)', display:'flex', alignItems:'center'}}>{day}</div>
                   {scheduleGrid[day].map((mode, hour) => (
                     <div
                       key={hour}
                       onClick={() => cycleMode(day, hour)}
                       title={`${day} ${String(hour).padStart(2,'0')}:00 - ${modes[mode]?.label}`}
                       style={{
                         height:'18px', borderRadius:'2px', cursor:'pointer',
                         background: modes[mode]?.color || modes.off.color,
                         border:'1px solid rgba(255,255,255,0.03)',
                         transition:'all 0.15s'
                       }}
                     />
                   ))}
                 </div>
               ))}
             </div>
            );
          })()}

          {/* AUDIT */}
          {activeNav === 'cfgAudit' && (
            <div>
              <div className="config-section-title">Nhật Ký Hệ thống (Audit Logs)</div>
              <div style={{display:'flex', gap:'6px', marginBottom:'8px'}}>
                <input className="config-input" placeholder="Tìm kiếm log..." style={{flex:1}} />
                <select className="config-select"><option>Tất cả</option><option>Đăng nhập</option><option>Cấu hình</option><option>Cảnh báo</option></select>
                <div className="config-btn">Xuất CSV</div>
              </div>
              <table className="lpr-table">
                <thead><tr><th style={{textAlign:'left', paddingLeft:10}}>Thời gian</th><th style={{textAlign:'left'}}>User</th><th style={{textAlign:'left'}}>Hành động</th><th style={{textAlign:'left'}}>Lệnh thực thi & Chi tiết</th></tr></thead>
                <tbody>
                  {auditLogs.length > 0 ? auditLogs.map((log, idx) => (
                    <tr key={log.id || idx}>
                      <td style={{fontFamily:"'Share Tech Mono',monospace", fontSize:'11px', padding:'10px', color:'var(--text-secondary)'}}>{log.created_at ? new Date(log.created_at * 1000).toLocaleString('vi-VN') : '-'}</td>
                      <td style={{fontWeight:'bold', color:'var(--accent)'}}>{log.username || `User ${log.user_id}`}</td>
                      <td style={{fontSize:'12px', color:'var(--warn)'}}>{log.action || '-'}</td>
                      <td style={{fontSize:'12px', maxWidth:'280px', textOverflow:'ellipsis', overflow:'hidden'}} title={log.details}>{log.details || '-'}</td>
                    </tr>
                  )) : (
                    <tr><td colSpan="4" style={{textAlign:'center', padding:'20px', color:'var(--text-dim)'}}>Đang tải lịch sử các hành động quản trị viên...</td></tr>
                  )}
                </tbody>
              </table>
            </div>
          )}


          {/* ═══════════ COUNTER ═══════════ */}
          {activeNav === 'cfgCounter' && (
            <div>
              <div className="config-section-title">Cấu Hình Đếm Người / Xe (Line Crossing)</div>
              <div style={{display:'flex', gap:'8px', marginBottom:'12px'}}>
                <div className="config-btn" onClick={() => {
                  setCounterZones(prev => [...prev, { id: Date.now(), name: `Zone ${prev.length+1}`, camera: cameras[0]?.name || 'CAM-01', type: 'bidirectional', in: 0, out: 0, enabled: true }]);
                  showToast('Đã thêm vùng đếm mới', 'success');
                }}>+ Thêm Vùng Đếm</div>
              </div>

              <div style={{display:'grid', gridTemplateColumns:'repeat(auto-fill, minmax(280px, 1fr))', gap:'10px'}}>
                {counterZones.map(zone => (
                  <div key={zone.id} style={{background:'var(--bg-card)', border:'1px solid var(--border)', borderRadius:'6px', padding:'14px', position:'relative'}}>
                    <div style={{display:'flex', justifyContent:'space-between', alignItems:'center', marginBottom:'10px'}}>
                      <div style={{fontFamily:"'Rajdhani',sans-serif", fontWeight:700, fontSize:'13px', letterSpacing:'1px'}}>{zone.name}</div>
                      <div className={`toggle ${zone.enabled ? 'on' : ''}`} onClick={() => {
                        setCounterZones(prev => prev.map(z => z.id === zone.id ? {...z, enabled: !z.enabled} : z));
                        setIsDirty(true);
                      }}></div>
                    </div>
                    <div style={{fontSize:'10px', color:'var(--text-secondary)', marginBottom:'8px'}}>
                      📹 {zone.camera} · <span style={{color:'var(--accent)'}}>{zone.type === 'vehicle' ? '🚗 Xe' : '🚶 Người'}</span>
                    </div>
                    <div style={{display:'flex', gap:'8px'}}>
                      <div style={{flex:1, background:'rgba(50,232,39,0.08)', border:'1px solid rgba(50,232,39,0.2)', borderRadius:'4px', padding:'8px', textAlign:'center'}}>
                        <div style={{fontSize:'20px', fontFamily:"'Share Tech Mono',monospace", fontWeight:700, color:'var(--accent3)'}}>{zone.in}</div>
                        <div style={{fontSize:'9px', color:'var(--text-secondary)', letterSpacing:'1px'}}>VÀO ↓</div>
                      </div>
                      <div style={{flex:1, background:'rgba(255,48,96,0.08)', border:'1px solid rgba(255,48,96,0.2)', borderRadius:'4px', padding:'8px', textAlign:'center'}}>
                        <div style={{fontSize:'20px', fontFamily:"'Share Tech Mono',monospace", fontWeight:700, color:'var(--danger)'}}>{zone.out}</div>
                        <div style={{fontSize:'9px', color:'var(--text-secondary)', letterSpacing:'1px'}}>RA ↑</div>
                      </div>
                    </div>
                    <div style={{display:'flex', justifyContent:'flex-end', marginTop:'8px'}}>
                      <div className="config-btn danger" style={{padding:'3px 8px', fontSize:'9px'}} onClick={() => {
                        setCounterZones(prev => prev.filter(z => z.id !== zone.id));
                        showToast('Đã xóa vùng đếm', 'warn');
                      }}>Xóa</div>
                    </div>
                  </div>
                ))}
              </div>
              {counterZones.length === 0 && <div style={{textAlign:'center', padding:'30px', color:'var(--text-dim)'}}>Chưa có vùng đếm. Bấm "+ Thêm Vùng Đếm".</div>}
            </div>
          )}

          {/* ═══════════ ZONES / ROI ═══════════ */}
          {activeNav === 'cfgZones' && (
            <div>
              <div className="config-section-title">Vùng Giám Sát ROI (Region of Interest)</div>
              <div style={{display:'flex', gap:'8px', marginBottom:'12px'}}>
                <div className="config-btn" onClick={() => setEditingCameraForZone(cameras[0]?.id || 1)}>
                   + Vẽ Vùng ROI Mới
                </div>
              </div>

              <div style={{display:'grid', gridTemplateColumns:'repeat(auto-fill, minmax(280px, 1fr))', gap:'10px'}}>
                {zones.map(zone => (
                  <div key={zone.zone_id} className="glass" style={{padding:'14px', borderRadius:'8px', border:'1px solid var(--border)'}}>
                    <div style={{display:'flex', justifyContent:'space-between', alignItems:'center', marginBottom:'10px'}}>
                       <div style={{fontFamily:'Rajdhani', fontWeight:700}}>{zone.name}</div>
                       <div className="config-badge active">ID: {zone.zone_id}</div>
                    </div>
                    <div style={{fontSize:'11px', color:'var(--text-secondary)', marginBottom:'10px'}}>
                       📹 Camera ID: {zone.camera_id} · Điểm: {zone.points?.length || 0}
                    </div>
                    <div style={{display:'flex', gap:'5px'}}>
                        <button className="grid-btn" style={{flex:1}} onClick={() => setEditingCameraForZone(zone.camera_id)}>SỬA VÙNG</button>
                        <button className="grid-btn danger" style={{padding:'5px', color:'var(--danger)'}} onClick={() => {
                           apiClient.deleteZone(zone.zone_id).then((res) => {
                             if (res.success) refreshZones();
                           });
                        }}>XÓA</button>
                    </div>
                  </div>
                ))}
              </div>
              {zones.length === 0 && <div style={{textAlign:'center', padding:'30px', color:'var(--text-dim)'}}>Chưa có vùng ROI. Bấm "+ Vẽ Vùng ROI Mới".</div>}
            </div>
          )}

          {/* ═══════════ ROLES / PERMISSIONS ═══════════ */}
          {activeNav === 'cfgRoles' && (
            <div>
              <div className="config-section-title">Phân Quyền Hệ Thống (Role-Based Access)</div>
              <div style={{display:'grid', gridTemplateColumns:'repeat(auto-fill, minmax(280px, 1fr))', gap:'12px'}}>
                {rolesData.map(role => {
                  const allPerms = ['cameras','playback','settings','users','ai','analytics','ppe','events','backup','audit','api'];
                  return (
                    <div key={role.id} style={{background:'var(--bg-card)', border:'1px solid var(--border)', borderRadius:'6px', padding:'16px', position:'relative'}}>
                      <div style={{position:'absolute', top:0, left:0, right:0, height:'3px', background:role.color, borderRadius:'6px 6px 0 0'}}></div>
                      <div style={{display:'flex', justifyContent:'space-between', alignItems:'center', marginBottom:'6px', marginTop:'4px'}}>
                        <div style={{fontFamily:"'Rajdhani',sans-serif", fontWeight:700, fontSize:'15px', color:role.color, letterSpacing:'1px'}}>{role.name}</div>
                        <span className="config-badge" style={{background:`${role.color}15`, color:role.color, border:`1px solid ${role.color}44`}}>{role.count} user</span>
                      </div>
                      <div style={{fontSize:'11px', color:'var(--text-secondary)', marginBottom:'12px'}}>{role.desc}</div>

                      <div style={{fontSize:'9px', color:'var(--text-dim)', letterSpacing:'1px', marginBottom:'6px', fontFamily:"'Rajdhani',sans-serif", fontWeight:700}}>QUYỀN HẠN</div>
                      <div style={{display:'flex', flexWrap:'wrap', gap:'4px'}}>
                        {allPerms.map(p => {
                          const has = role.perms.includes(p);
                          return (
                            <span key={p} style={{
                              padding:'2px 6px', borderRadius:'3px', fontSize:'9px',
                              background: has ? `${role.color}15` : 'rgba(255,255,255,0.02)',
                              color: has ? role.color : 'var(--text-dim)',
                              border: `1px solid ${has ? role.color+'33' : 'var(--border)'}`,
                              fontFamily:"'Share Tech Mono',monospace",
                              textTransform:'uppercase'
                            }}>{has ? '✓' : '✕'} {p}</span>
                          );
                        })}
                      </div>
                    </div>
                  );
                })}
              </div>
              <div style={{marginTop:'15px', padding:'12px', background:'var(--bg-card)', border:'1px solid var(--border)', borderRadius:'4px', display:'flex', alignItems:'center', gap:'10px'}}>
                <svg viewBox="0 0 14 14" fill="none" stroke="var(--text-dim)" strokeWidth="1.5" style={{width:'16px', height:'16px', flexShrink:0}}><circle cx="7" cy="7" r="5.5"/><path d="M7 4.5v3M7 9.5h.01"/></svg>
                <span style={{fontSize:'11px', color:'var(--text-secondary)'}}>Để tạo Role mới hoặc chỉnh sửa quyền, sử dụng API: <code style={{color:'var(--accent)', fontSize:'10px'}}>POST /api/roles</code></span>
              </div>
            </div>
          )}

          {/* ═══════════ INTEGRATION ═══════════ */}
          {activeNav === 'cfgIntegration' && (
            <div>
              <div className="config-section-title">Tích Hợp Hệ Thống Bên Thứ Ba</div>
              <div style={{display:'grid', gridTemplateColumns:'repeat(auto-fill, minmax(280px, 1fr))', gap:'12px'}}>
                {[
                  { name: 'Active Directory / LDAP', icon: '🔐', desc: 'Đồng bộ tài khoản từ Windows Domain Controller', status: 'available', statusLabel: 'Sẵn sàng' },
                  { name: 'Zalo OA Notification', icon: '💬', desc: 'Gửi cảnh báo qua Zalo Official Account API', status: 'connected', statusLabel: 'Đã kết nối' },
                  { name: 'Telegram Bot', icon: '📱', desc: 'Push thông báo realtime qua Bot API', status: 'available', statusLabel: 'Sẵn sàng' },
                  { name: 'ONVIF Discovery', icon: '📡', desc: 'Tự động tìm và đăng ký camera IP chuẩn ONVIF', status: 'connected', statusLabel: 'Hoạt động' },
                  { name: 'Amazon S3 / MinIO', icon: '☁️', desc: 'Lưu trữ snapshot & video cloud phân tán', status: settings.backup_cloud ? 'connected' : 'available', statusLabel: settings.backup_cloud ? 'Đã kết nối' : 'Chưa cấu hình' },
                  { name: 'SMTP Email Server', icon: '📧', desc: 'Gửi email cảnh báo với ảnh chụp đính kèm', status: settings.alert_email_enabled ? 'connected' : 'available', statusLabel: settings.alert_email_enabled ? 'Hoạt động' : 'Tắt' },
                  { name: 'Database Export (PostgreSQL)', icon: '🗃️', desc: 'Đồng bộ dữ liệu sự kiện sang DB ngoài', status: 'available', statusLabel: 'Sẵn sàng' },
                  { name: 'Access Control (HID/ZKTeco)', icon: '🚪', desc: 'Tích hợp khóa cửa, barrier tự động qua FaceID', status: 'available', statusLabel: 'Sẵn sàng' },
                ].map((item, idx) => (
                  <div key={idx} style={{
                    background:'var(--bg-card)', border:'1px solid var(--border)', borderRadius:'6px', padding:'14px',
                    display:'flex', gap:'12px', alignItems:'flex-start', transition:'border 0.2s',
                    cursor:'pointer'
                  }} onMouseEnter={e => e.currentTarget.style.borderColor = 'var(--accent)'} onMouseLeave={e => e.currentTarget.style.borderColor = 'var(--border)'}>
                    <div style={{fontSize:'24px', width:'36px', height:'36px', display:'flex', alignItems:'center', justifyContent:'center', background:'var(--bg-hover)', borderRadius:'6px', flexShrink:0}}>{item.icon}</div>
                    <div style={{flex:1, minWidth:0}}>
                      <div style={{display:'flex', justifyContent:'space-between', alignItems:'center', marginBottom:'4px'}}>
                        <div style={{fontFamily:"'Rajdhani',sans-serif", fontWeight:700, fontSize:'12px', letterSpacing:'0.5px'}}>{item.name}</div>
                        <span className={`config-badge ${item.status === 'connected' ? 'active' : 'inactive'}`} style={{fontSize:'8px'}}>{item.statusLabel}</span>
                      </div>
                      <div style={{fontSize:'10px', color:'var(--text-secondary)', lineHeight:1.4}}>{item.desc}</div>
                    </div>
                  </div>
                ))}
              </div>
            </div>
          )}

          {/* SYSTEM STATS */}
          {activeNav === 'cfgSystem' && (() => {
            const uptimeSec = systemStats.uptime_seconds || 0;
            const days = Math.floor(uptimeSec / 86400);
            const hrs = Math.floor((uptimeSec % 86400) / 3600);
            const mins = Math.floor((uptimeSec % 3600) / 60);
            return (
            <div>
               <div className="config-section-title">Máy Chủ & Tài Nguyên Hệ Thống</div>
               <div className="config-section">
                 <div className="config-row"><div className="config-label">Phiên bản C++ Core API</div><div className="config-val">v2.1 Ultimate Edition</div></div>
                 <div className="config-row"><div className="config-label">Chip Xử Lý Cơ Sở (CPU)</div><div className="config-val" style={{color:'var(--accent)'}}>{systemStats.cpu_model || 'Loading...'}</div></div>
                 <div className="config-row"><div className="config-label">Tải hệ thống CPU</div><div className="config-val">{Math.round(systemStats.cpu_percent || 0)}%</div></div>
                 <div className="config-row"><div className="config-label">Bộ nhớ tạm (RAM)</div><div className="config-val">{systemStats.ram_used_gb?.toFixed(1) || 0} / {systemStats.ram_total_gb?.toFixed(1) || 0} GB <span style={{fontSize:'10px', marginLeft:'5px'}} className="alarm-sev medium">{(systemStats.ram_percent || 0).toFixed(1)}%</span></div></div>
                 <div className="config-row"><div className="config-label">Tài Nguyên Đồ Họa VRAM (Graphic Card)</div><div className="config-val">{systemStats.gpu_memory_used_mb || 0} / {systemStats.gpu_memory_total_mb || 0} MB <span style={{fontSize:'10px', marginLeft:'5px'}} className="alarm-sev critical">{(systemStats.gpu_percent || 0).toFixed(1)}%</span></div></div>
                 <div className="config-row"><div className="config-label">Độ chính xác Model AI (Global Confidence)</div><div className="config-val" style={{color:'var(--accent3)'}}>{(systemStats.ai_accuracy || 0).toFixed(1)}%</div></div>
                 <div className="config-row"><div className="config-label">Thời gian máy rảnh (Uptime)</div><div className="config-val" style={{fontFamily:"'Share Tech Mono',monospace"}}>{days}d {hrs}h {mins}m</div></div>
                 <div className="config-row"><div className="config-label">Tổng Camera Cắm Nóng</div><div className="config-val">{cameras.length} thiết bị</div></div>
                 <div className="config-row"><div className="config-label">Thông tin giấy phép VMS</div><div><span className="config-badge active" style={{border:'1px solid var(--accent3)', color:'var(--accent3)', background:'rgba(50,232,39,0.1)'}}>Enterprise Unlimited</span></div></div>
               </div>
            </div>
          );})()}
          
          {/* BACKUP */}
          {activeNav === 'cfgBackup' && (
            <div>
              <div className="config-section-title">Sao Lưu & Phục Hồi An Toàn</div>
              <div style={{background:'var(--bg-card)', border:'1px solid rgba(50,232,39,0.2)', borderRadius:'4px', padding:'15px', marginBottom:'15px', display:'flex', alignItems:'center', gap:'15px'}}>
                <div style={{width:'40px', height:'40px', borderRadius:'50%', background:'rgba(50,232,39,0.1)', border:'1px solid rgba(50,232,39,0.4)', display:'flex', alignItems:'center', justifyContent:'center', fontSize:'20px', flexShrink:0, color:'var(--accent3)'}}>✓</div>
                <div><div style={{fontSize:'12px', color:'var(--accent3)', fontFamily:'Rajdhani, sans-serif', fontWeight:700}}>Tệp lưu trữ SQL & Hình ảnh được đảm bảo toàn vẹn</div><div style={{fontSize:'10px', color:'var(--text-secondary)'}}>Backup Job chạy lúc 06:00 sáng. Tổng dung lượng chiếm dụng: {systemStats.disk_used_gb?.toFixed(1)} GB.</div></div>
                <div className="config-btn" style={{marginLeft:'auto', fontWeight:'bold', border:'1px solid var(--accent3)', color:'var(--accent3)'}} onClick={() => showToast('Đang khởi tạo tiến trình nén Zip...', 'info')}>Chạy tiến trình nén Zip</div>
              </div>
              <div className="config-section">
                <div className="config-row"><div><div className="config-label">Thời điểm Backup tự động (Cron-Job)</div></div><select className="config-select" value={settings.backup_schedule} onChange={e => handleChange('backup_schedule', e.target.value)}><option value="Hàng giờ">Hàng giờ (Không khuyến nghị)</option><option value="Hàng ngày 06:00">Hàng ngày 06:00 Sáng</option></select></div>
                <div className="config-row"><div><div className="config-label">Đẩy lên Server nội bộ (NAS Storage)</div><div className="config-desc">Giao thức FTP / SMB SMBv3_\\nas\vms-backup</div></div><div className={`toggle ${settings.backup_nas ? 'on' : ''}`} onClick={() => handleToggle('backup_nas')}></div></div>
                <div className="config-row"><div><div className="config-label">Đồng bộ Cloud phi tập trung (Amazon S3)</div><div className="config-desc">Bucket liên kết: s3://vms-backup-bucket</div></div><div className={`toggle ${settings.backup_cloud ? 'on' : ''}`} onClick={() => handleToggle('backup_cloud')}></div></div>
              </div>
            </div>
          )}
          {/* BATCH IMPORT MODAL */}
          {importModalOpen && (
            <BatchImportModal 
              onClose={() => setImportModalOpen(false)} 
              onImported={async () => {
                await refreshCameras();
                showToast('Đã cập nhật danh sách camera sau khi import thành công', 'success');
              }}
            />
          )}

        </div>
      </div>

      {/* AI Zone Editor Modal */}
      {editingCameraForZone && (
        <AiRuleEditor 
          cameraId={editingCameraForZone} 
          onClose={() => setEditingCameraForZone(null)} 
          onSave={(zoneData) => {
             apiClient.addZone({ ...zoneData, name: `Vùng ${zones.length + 1}` }).then(res => {
                if (res.success) {
                   refreshZones();
                   setEditingCameraForZone(null);
                   showToast('Đã lưu vùng giám sát AI thành công!', 'success');
                } else {
                   showToast('Lỗi lưu vùng: ' + (res.error || 'Unknown'), 'error');
                }
             });
          }}
        />
      )}

      {/* Toast Notification */}
      {toast && <Toast key={toast.key} message={toast.message} type={toast.type} onClose={() => setToast(null)} />}

      {/* 2FA Modal */}
      {show2FAModal && (
        <TwoFactorSetupModal 
          onClose={() => setShow2FAModal(false)} 
          onEnabled={async () => {
             await refreshUsers();
             setShow2FAModal(false);
             showToast('Đã kích hoạt bảo mật 2 lớp (2FA) thành công!', 'success');
          }} 
        />
      )}

      {/* PTZ Patrol Modal */}
      {patrolCameraId && (
        <PtzPatrolModal 
          cameraId={patrolCameraId} 
          onClose={() => setPatrolCameraId(null)} 
        />
      )}
    </div>
  );
};

export default SettingsView;

