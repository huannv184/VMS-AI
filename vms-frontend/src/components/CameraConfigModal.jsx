import React, { useState, useEffect, useRef } from 'react';
import { Search, Server, Activity } from 'lucide-react';
import apiClient from '../api/apiClient';
import useVmsStore from '../store/useVmsStore';

const trim = (value) => (value || '').trim();
const isIpv4 = (value) => /^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$/.test(value);
const isHostname = (value) => /^(?=.{1,253}$)(?:(?!-)[A-Za-z0-9-]{1,63}(?<!-)\.)*(?:(?!-)[A-Za-z0-9-]{1,63}(?<!-))$/.test(value);

const CameraConfigModal = ({ onClose, initialData }) => {
  const addCamera = useVmsStore((state) => state.addCamera);
  const updateCameraStore = useVmsStore((state) => state.updateCamera);
  const cameras = useVmsStore((state) => state.cameras);
  const [loading, setLoading] = useState(false);
  
  // Quick Scan
  const [scanning, setScanning] = useState(false);
  
  // Deep Scan
  const [deepScanning, setDeepScanning] = useState(false);
  const [scanProgress, setScanProgress] = useState(0);
  const [scanPhase, setScanPhase] = useState('');
  const [scanId, setScanId] = useState(null);
  
  const [discoveredDevices, setDiscoveredDevices] = useState([]);
  const discoveredCountRef = useRef(0);
  const deepScanIntervalRef = useRef(null);
  const [networkRange, setNetworkRange] = useState('192.168.1.0/24');
  const [sites, setSites] = useState([]);
  
  const [formData, setFormData] = useState({
    name: initialData?.name || 'Camera mới',
    ip: '',
    port: '554',
    user: 'admin',
    pass: '',
    manufacturer: 'ONVIF',
    description: initialData?.description || 'Khu vực giám sát mới',
    protocol: 'RTSP / H264',
    recommended_path: '/stream',
    site_id: initialData?.site_id || -1
  });

  // Hardware event-subscription health snapshot (ONVIF PullPoint / Axis
  // eventfeed / Dahua attach / Hanwha poll / Hikvision adapter). Only
  // meaningful when editing an existing camera; new cameras have no
  // CameraEventService session yet. Polled every 5s while the modal is open.
  // See backend endpoint added in closeouts_bundle_2026_05_12.
  const [subStatus, setSubStatus] = useState(null);
  useEffect(() => {
    if (!initialData?.id) return;
    let cancelled = false;
    const tick = async () => {
      try {
        const res = await apiClient.getEventSubscriptionStatus?.(initialData.id);
        if (!cancelled && res?.success) setSubStatus(res.data || null);
      } catch (_) { /* ignore */ }
    };
    tick();
    const id = setInterval(tick, 5000);
    return () => { cancelled = true; clearInterval(id); };
  }, [initialData?.id]);

  useEffect(() => {
    if (initialData && initialData.rtsp_url) {
      const parsed = parseRtspUrl(initialData.rtsp_url);
      if (parsed) {
        setFormData(prev => ({
          ...prev,
          ip: parsed.ip,
          port: parsed.port,
          user: parsed.user,
          pass: parsed.pass,
          recommended_path: parsed.path
        }));
      }
    }
  }, [initialData]);

  const parseRtspUrl = (url) => {
    if (!url) return null;
    const match = url.match(/rtsp:\/\/([^:]+):([^@]+)@([^:/]+)(?::(\d+))?(\/.*)/);
    if (match) {
      return {
        user: match[1],
        pass: match[2],
        ip: match[3],
        port: match[4] || '554',
        path: match[5]
      };
    }
    const matchSimple = url.match(/rtsp:\/\/([^:/]+)(?::(\d+))?(\/.*)/);
    if (matchSimple) {
      return {
        ip: matchSimple[1],
        port: matchSimple[2] || '554',
        path: matchSimple[3]
      };
    }
    return null;
  };

  useEffect(() => {
    const fetchSites = async () => {
      try {
        const res = await apiClient.getSites();
        if (res.success) setSites(res.data?.sites || []);
      } catch (err) {
        console.warn('Failed to fetch sites:', err);
      }
    };
    fetchSites();

    if (window.__vms_auto_scan) {
      scanNetworkDeep();
      window.__vms_auto_scan = false;
    }

    return () => {
      if (deepScanIntervalRef.current) {
        clearInterval(deepScanIntervalRef.current);
        deepScanIntervalRef.current = null;
      }
    };
  }, []);

  const handleChange = (e) => {
    const { name, value } = e.target;
    setFormData(prev => ({ ...prev, [name]: value }));
  };

  const scanOnvifQuick = async () => {
    setScanning(true);
    try {
      const res = await apiClient.discoverOnvif();
      const devices = res.data?.devices || [];
      if (res.success && Array.isArray(devices) && devices.length > 0) {
        setDiscoveredDevices(prev => {
           const existingIps = new Set(prev.map(d => d.ip));
           const newDevs = devices.filter(d => !existingIps.has(d.ip || (d.service_url && d.service_url.split('/')[2].split(':')[0])));
           
           const mappedNew = newDevs.map(d => {
             let ip = d.ip;
             if (!ip && d.service_url) {
                const match = d.service_url.match(/https?:\/\/([^\/:]+)/);
                if (match) ip = match[1];
             }
             return {
                ip: ip,
                port: 80,
                model: d.model || d.name,
                manufacturer: d.manufacturer,
                recommended_path: d.recommended_rtsp_path || '/stream',
                is_auth_success: false
             };
           });
           const merged = [...prev, ...mappedNew];
           discoveredCountRef.current = merged.length;
           return merged;
        });
      } else if (discoveredCountRef.current === 0) {
        alert(res.error || "Không tìm thấy camera ONVIF nào trong mạng LAN.");
      }
    } catch (e) {
      alert("Lỗi khi quét mạng LAN.");
    } finally {
      setScanning(false);
    }
  };

  const scanNetworkDeep = async () => {
    if (deepScanning) return;
    setDeepScanning(true);
    setScanProgress(0);
    setScanPhase('');
    setDiscoveredDevices([]);
    discoveredCountRef.current = 0;
    
    try {
      const res = await apiClient.startNetworkScan({ network_range: networkRange, enable_brute_force: true });
      if (res.success && res.data?.scan_id) {
        const sid = res.data.scan_id;
        setScanId(sid);

        // Hard timeout (5 minutes) so a hung scan doesn't poll forever
        const startedAt = Date.now();
        const MAX_SCAN_MS = 5 * 60 * 1000;

        deepScanIntervalRef.current = setInterval(async () => {
           if (Date.now() - startedAt > MAX_SCAN_MS) {
             clearInterval(deepScanIntervalRef.current);
             deepScanIntervalRef.current = null;
             setDeepScanning(false);
             setScanPhase('timeout');
             alert('Quá trình quét bị treo quá 5 phút – đã dừng để tránh poll vô hạn.');
             return;
           }
           const st = await apiClient.getNetworkScanStatus(sid);
           if (st.success) {
              const scanState = st.data || {};
              setScanProgress(scanState.progress || 0);
              if (scanState.phase) {
                const phaseLabels = { onvif: 'ONVIF Discovery', sunapi: 'SUNAPI (Hanwha)', ip_scan: 'IP Scan', done: 'Hoàn tất' };
                setScanPhase(phaseLabels[scanState.phase] || scanState.phase);
              }
              if (scanState.results && scanState.results.length > 0) {
                 const mapped = scanState.results.map(r => ({
                    ip: r.ip,
                    port: r.port,
                    manufacturer: r.vendor === 'generic_cctv' ? 'ONVIF' : (r.vendor || 'Unknown'),
                    model: r.model || 'Network Camera',
                    user: r.username,
                    pass: r.password,
                    recommended_path: r.rtsp_url ? r.rtsp_url.replace(/rtsp:\/\/.*@.*:\d+/, '') : '/stream',
                    is_auth_success: r.is_auth_success,
                    discovery_method: r.discovery_method || 'ip_scan',
                    mac_address: r.mac_address
                 }));
                 setDiscoveredDevices(mapped);
                 discoveredCountRef.current = mapped.length;
              }
              if (scanState.status === 'completed' || scanState.status === 'stopped') {
                 clearInterval(deepScanIntervalRef.current);
                 deepScanIntervalRef.current = null;
                 setDeepScanning(false);
                 setScanProgress(100);
                 setScanPhase('done');
              }
           }
        }, 1500);
      } else {
         setDeepScanning(false);
         alert(res.error || "Không thể khởi động tiến trình quét từ máy chủ.");
      }
    } catch(e) {
      setDeepScanning(false);
      alert("Lỗi kết nối khi quét mạng: " + (e.message || e));
    }
  };

  const stopDeepScan = async () => {
     if (scanId && deepScanning) {
        await apiClient.stopNetworkScan(scanId);
        if (deepScanIntervalRef.current) {
          clearInterval(deepScanIntervalRef.current);
          deepScanIntervalRef.current = null;
        }
        setDeepScanning(false);
     }
  };

  const verifyConnection = async () => {
     if (!trim(formData.ip)) {
        alert("Vui lòng nhập IP để kiểm tra"); return;
     }
     
     setLoading(true);
     try {
       const payload = {
          host: formData.ip,
          username: formData.user,
          password: formData.pass,
          http_port: 80,
          rtsp_port: parseInt(formData.port) || 554,
       };
       const res = await apiClient.discoverCamera(payload);
       
       if (res.success && res.data?.cameras && res.data.cameras.length > 0) {
          const streamObj = res.data.cameras.find(c => c.channel_id === 1) || res.data.cameras[0];
          let updatedPath = streamObj.rtsp_main || formData.recommended_path;
          
          if (updatedPath && updatedPath.includes('rtsp://')) {
              try {
                 const u = new URL(updatedPath);
                 updatedPath = u.pathname + u.search;
              } catch(e) {}
          }
          
          setFormData(prev => ({ 
             ...prev, 
             recommended_path: updatedPath,
             manufacturer: res.data.brand && res.data.brand !== 'Unknown' ? res.data.brand : prev.manufacturer
          }));
          alert(`✅ Kiểm tra thành công!\nLuồng chính: ${updatedPath}\nHãng: ${res.data.brand || 'Unknown'}`);
       } else {
          alert(`❌ ${res.error || 'Không thể xác thực cấu hình API/ONVIF với thông tin này!'}`);
       }
     } catch(e) {
       alert("Lỗi kết nối đến Backend hoặc Timeout");
     } finally {
       setLoading(false);
     }
  };

  const selectDevice = (dev) => {
    setFormData(prev => ({
      ...prev,
      name: dev.model && dev.model !== 'Network Camera' ? `${dev.manufacturer} ${dev.model}` : `Camera ${dev.ip}`,
      ip: dev.ip,
      port: '554', 
      user: dev.user || prev.user,
      pass: dev.pass || prev.pass,
      manufacturer: dev.manufacturer === 'generic_cctv' ? 'ONVIF' : (dev.manufacturer || 'ONVIF'),
      recommended_path: dev.recommended_path || '/stream'
    }));
  };

  const handleSave = async () => {
    const name = trim(formData.name);
    const ip = trim(formData.ip);
    const port = Number.parseInt(formData.port, 10);
    const path = trim(formData.recommended_path || '/stream');
    const username = trim(formData.user);

    const pathClean = path.replace(/^\/+/, ''); // strip leading slashes
    const isWebcamMode = pathClean === 'webcam' || pathClean === '0';

    if (!name) {
      alert("Vui lòng nhập tên camera.");
      return;
    }
    if (!isWebcamMode) {
      if (!ip || (!isIpv4(ip) && !isHostname(ip))) {
        alert("Địa chỉ IP hoặc hostname không hợp lệ.");
        return;
      }
      if (!Number.isInteger(port) || port < 1 || port > 65535) {
        alert("Cổng RTSP không hợp lệ.");
        return;
      }
    }

    setLoading(true);
    try {
      let finalRtspUrl = path || '/stream';
      
      if (isWebcamMode) {
         finalRtspUrl = pathClean; // Send exactly 'webcam' or '0' to backend
      } else {
          const isAbsoluteUrl = finalRtspUrl.startsWith('rtsp://') || finalRtspUrl.startsWith('http://') || finalRtspUrl.startsWith('https://') || finalRtspUrl.startsWith('ws://');
          
          if (isAbsoluteUrl) {
              if (username && finalRtspUrl.indexOf('@') === -1) {
                  const authPart = `${encodeURIComponent(username)}:${encodeURIComponent(formData.pass || '')}@`;
                  finalRtspUrl = finalRtspUrl.replace(/^(rtsp:\/\/)/, `$1${authPart}`);
              }
          } else {
              const pathSegment = finalRtspUrl.startsWith('/') ? finalRtspUrl : '/' + finalRtspUrl;
              const authSegment = username
                ? `${encodeURIComponent(username)}:${encodeURIComponent(formData.pass || '')}@`
                : '';
              finalRtspUrl = `rtsp://${authSegment}${ip}:${port}${pathSegment}`;
          }
      }
      
      const payload = {
        name,
        rtsp_url: finalRtspUrl,
        description: trim(formData.description),
        is_active: true,
        site_id: parseInt(formData.site_id) || -1
      };

      if (initialData?.id) {
        const result = await apiClient.updateCamera(initialData.id, payload);
        if (result.success) {
          updateCameraStore(result.data);
          onClose();
        } else {
          alert(result.error || "Không thể cập nhật camera.");
        }
      } else {
        const result = await apiClient.addCamera(payload);
        if (result.success) {
          addCamera(result.data);
          onClose();
        } else {
          alert(result.error || "Không thể thêm camera. Vui lòng kiểm tra lại kết nối backend.");
        }
      }
    } catch (error) {
      console.error("Lỗi khi thêm camera:", error);
      alert("Đã xảy ra lỗi trong quá trình lưu.");
    } finally {
      setLoading(false);
    }
  };

  return (
    <div style={{position:'fixed', top:0, left:0, right:0, bottom:0, background:'rgba(0,0,0,0.85)', zIndex:1000, display:'flex', alignItems:'center', justifyContent:'center'}}>
      <div style={{width:'950px', height:'650px', background:'var(--bg-panel)', border:'1px solid var(--border)', borderRadius:'4px', display:'flex', flexDirection:'column', boxShadow:'0 10px 40px rgba(0,0,0,0.9)'}}>
        
        {/* Header */}
        <div style={{padding:'15px 20px', borderBottom:'1px solid var(--border)', display:'flex', justifyContent:'space-between', alignItems:'center'}}>
          <div style={{fontFamily:"'Rajdhani',sans-serif", fontSize:'18px', fontWeight:700, color:'var(--text-primary)', letterSpacing:'1px', display:'flex', alignItems:'center'}}>
             <Server size={18} style={{marginRight: 10, color:'var(--accent)'}}/> {initialData ? 'CẬP NHẬT CAMERA' : 'THIẾT LẬP CAMERA'} & KHÔNG GIAN ROI
          </div>
          <div onClick={onClose} style={{cursor:'pointer', color:'var(--text-secondary)'}}>✕</div>
        </div>
        
        <div style={{display:'flex', flex:1, overflow:'hidden'}}>
          {/* Left: Form & Scanner */}
          <div style={{width:'360px', borderRight:'1px solid var(--border)', padding:'20px', overflowY:'auto', display:'flex', flexDirection:'column', gap:'15px'}}>
            
            {/* Network Scan */}
            <div style={{background:'var(--bg-card)', padding:'12px', borderRadius:'4px', border:'1px solid var(--border)'}}>
               <div style={{fontSize:'11px', color:'var(--text-dim)', marginBottom:'8px', fontFamily:"'Rajdhani',sans-serif", fontWeight:700, display:'flex', justifyContent:'space-between'}}>
                  <span>CÔNG CỤ QUÉT MẠNG TỰ ĐỘNG</span>
                  {deepScanning && <span style={{color:'var(--accent3)'}}>{scanPhase ? `${scanPhase}` : 'Đang xử lý'} · {scanProgress}%</span>}
               </div>
               
               <div style={{display:'flex', gap:'5px', marginBottom:'10px'}}>
                  <input type="text" className="search-input" style={{flex:1, fontSize:'11px'}} value={networkRange} onChange={e => setNetworkRange(e.target.value)} placeholder="VD: 192.168.1.0/24"/>
               </div>

               <div style={{display:'flex', gap:'8px'}}>
                 <button onClick={scanOnvifQuick} disabled={scanning || deepScanning} className="config-btn" style={{flex:1, padding:'6px', fontSize:'11px', justifyContent:'center'}}>
                   {scanning ? '...' : 'QUÉT NHANH UDP'}
                 </button>
                 {!deepScanning ? (
                    <button onClick={scanNetworkDeep} disabled={scanning} className="config-btn" style={{flex:1, padding:'6px', fontSize:'11px', justifyContent:'center', background:'rgba(0,200,245,0.1)', borderColor:'var(--accent)'}}>
                      <Search size={12} style={{marginRight:4}}/> QUÉT SÂU (DEEP)
                    </button>
                 ) : (
                    <button onClick={stopDeepScan} className="config-btn danger" style={{flex:1, padding:'6px', fontSize:'11px', justifyContent:'center'}}>
                      DỪNG TIẾN TRÌNH
                    </button>
                 )}
               </div>
            </div>

            {discoveredDevices.length > 0 && (
              <div style={{background:'var(--bg-card)', padding:'10px', borderRadius:'4px', border:'1px dashed var(--border)'}}>
                <div style={{fontSize:'10px', color:'var(--text-dim)', marginBottom:'8px', display:'flex', justifyContent:'space-between'}}>
                   <span>KẾT QUẢ TÌM KIẾM ({discoveredDevices.length})</span>
                   <span style={{color:'var(--accent)'}}>CLICK ĐỂ CHỌN</span>
                </div>
                <div style={{maxHeight:'140px', overflowY:'auto', display:'flex', flexDirection:'column', gap:'5px', paddingRight:'5px'}}>
                  {discoveredDevices.map((dev, idx) => (
                    <div key={idx} onClick={() => selectDevice(dev)}
                      style={{
                        padding:'8px 10px', background:'rgba(255,255,255,0.03)', cursor:'pointer', 
                        borderRadius:'3px', fontSize:'11px', border:'1px solid rgba(255,255,255,0.05)', 
                        display:'flex', alignItems:'center', gap:'10px', transition:'all 0.2s'
                      }}
                    >
                      <div style={{fontWeight:'bold', color:'var(--text)', whiteSpace:'nowrap', overflow:'hidden', textOverflow:'ellipsis'}}>
                        {dev.ip} {dev.is_auth_success && <span style={{fontSize:'9px', color:'var(--accent3)', marginLeft:'5px'}}>[Đã Unlock]</span>}
                      </div>
                      <div style={{fontFamily:"'Share Tech Mono', monospace", color:'var(--accent)', fontSize:'10px', opacity:0.8}}>
                        {dev.manufacturer} {dev.model ? `· ${dev.model}` : ''}
                      </div>
                    </div>
                  ))}
                </div>
              </div>
            )}

            <div style={{height:'1px', background:'var(--border)', margin:'5px 0'}}></div>
            
            {/* Camera Parameters */}
            <div>
              <label style={{display:'block', fontSize:'11px', color:'var(--text-dim)', marginBottom:'6px', fontFamily:"'Rajdhani',sans-serif", fontWeight:700}}>TÊN CAMERA & HÃNG SẢN XUẤT</label>
              <div style={{display:'flex', gap:'5px'}}>
                 <input name="name" type="text" className="search-input" style={{flex:2}} value={formData.name} onChange={handleChange} />
                 <select name="manufacturer" className="search-input" style={{flex:1, padding:'4px'}} value={formData.manufacturer} onChange={handleChange}>
                   <option value="ONVIF">ONVIF</option>
                   <option value="Hikvision">Hikvision</option>
                   <option value="Dahua">Dahua</option>
                   <option value="Kbvision">KBVision</option>
                   <option value="Uniview">Uniview</option>
                   <option value="Axis">Axis</option>
                   <option value="Hanwha">Hanwha</option>
                   <option value="Bosch">Bosch</option>
                   <option value="Reolink">Reolink</option>
                   <option value="Milesight">Milesight</option>
                 </select>
              </div>
            </div>

            {/* Site Dropdown */}
            <div style={{marginTop:'5px'}}>
              <label style={{display:'block', fontSize:'11px', color:'var(--text-dim)', marginBottom:'6px', fontFamily:"'Rajdhani',sans-serif", fontWeight:700}}>GÁN VÀO CHI NHÁNH / VỊ TRÍ</label>
              <select name="site_id" className="search-input" style={{width:'100%', padding:'5px', color:'var(--accent3)'}} value={formData.site_id} onChange={handleChange}>
                <option value="-1">-- Không gán vị trí (Mặc định) --</option>
                {sites.map(s => (
                   <option key={s.id} value={s.id}>{s.name} {s.address ? ` - ${s.address}` : ''}</option>
                ))}
              </select>
            </div>

            <div style={{display:'flex', gap:'10px'}}>
              <div style={{flex:2}}>
                <label style={{display:'block', fontSize:'11px', color:'var(--text-dim)', marginBottom:'6px', fontFamily:"'Rajdhani',sans-serif", fontWeight:700}}>ĐỊA CHỈ IP / DOMAIN</label>
                <input name="ip" type="text" className="search-input" style={{width:'100%'}} value={formData.ip} onChange={handleChange} placeholder="192.168.1.108" />
              </div>
              <div style={{flex:1}}>
                <label style={{display:'block', fontSize:'11px', color:'var(--text-dim)', marginBottom:'6px', fontFamily:"'Rajdhani',sans-serif", fontWeight:700}}>PORT</label>
                <input name="port" type="text" className="search-input" style={{width:'100%'}} value={formData.port} onChange={handleChange} />
              </div>
            </div>

            <div style={{display:'flex', gap:'10px'}}>
              <div style={{flex:1}}>
                <label style={{display:'block', fontSize:'11px', color:'var(--text-dim)', marginBottom:'6px'}}>TÀI KHOẢN</label>
                <input name="user" type="text" className="search-input" style={{width:'100%'}} value={formData.user} onChange={handleChange} />
              </div>
              <div style={{flex:1}}>
                <label style={{display:'block', fontSize:'11px', color:'var(--text-dim)', marginBottom:'6px'}}>MẬT KHẨU</label>
                <input name="pass" type="password" className="search-input" style={{width:'100%'}} value={formData.pass} onChange={handleChange} />
              </div>
            </div>
            
            <div style={{marginTop:'5px'}}>
               <label style={{display:'block', fontSize:'11px', color:'var(--text-dim)', marginBottom:'6px'}}>RTSP PATH (Nâng cao)</label>
               <div style={{display:'flex', gap:'5px'}}>
                  <input name="recommended_path" type="text" className="search-input" style={{flex:1, color:'var(--accent)', fontFamily:"'Share Tech Mono', monospace"}} value={formData.recommended_path} onChange={handleChange} />
                  <button onClick={verifyConnection} disabled={loading} style={{background:'rgba(50,232,39,0.1)', border:'1px solid var(--accent3)', color:'var(--accent3)', padding:'0 10px', borderRadius:'3px', cursor:'pointer', fontSize:'11px', fontWeight:'bold'}}>
                    {loading ? '...' : 'KIỂM TRA'}
                  </button>
               </div>
            </div>
            
            {/* Hardware event subscription status — only shown when editing
                an existing camera. Operators use this to distinguish "scene
                quiet" from "subscription wedged" without tail-ing logs. */}
            {initialData?.id && subStatus && subStatus.listening && (() => {
              const stateColor = {
                active:  'var(--accent3, #32e827)',
                pending: 'var(--accent, #00c8f5)',
                failed:  'var(--danger, #ff3060)',
                stopped: 'var(--text-dim)',
              }[subStatus.state] || 'var(--text-dim)';
              const stateLabel = {
                active:  'ĐANG NHẬN',
                pending: 'ĐANG KẾT NỐI',
                failed:  'MẤT KẾT NỐI',
                stopped: 'TẮT',
              }[subStatus.state] || subStatus.state?.toUpperCase();
              const fmtSec = (s) => {
                if (s == null || s < 0) return '—';
                if (s < 60) return `${s}s`;
                if (s < 3600) return `${Math.floor(s/60)}m ${s%60}s`;
                return `${Math.floor(s/3600)}h ${Math.floor((s%3600)/60)}m`;
              };
              return (
                <div style={{
                  marginTop: 12, padding: 10,
                  background: 'rgba(0,200,245,0.04)',
                  border: '1px solid var(--border)',
                  borderRadius: 4, fontSize: 11,
                }}>
                  <div style={{display:'flex', justifyContent:'space-between', alignItems:'center', marginBottom: 6}}>
                    <span style={{color:'var(--text-secondary)', letterSpacing:'1px'}}>HARDWARE EVENT SUB ({subStatus.brand || 'unknown'})</span>
                    <span style={{
                      padding:'2px 8px', borderRadius:'3px',
                      background:`${stateColor}22`, color:stateColor,
                      fontFamily:"'Share Tech Mono',monospace", fontWeight:700,
                    }}>{stateLabel}</span>
                  </div>
                  <div style={{display:'grid', gridTemplateColumns:'1fr 1fr', gap:4, fontFamily:"'Share Tech Mono',monospace", color:'var(--text-primary)'}}>
                    <span>Sự kiện đã nhận: <b>{subStatus.total_events ?? 0}</b></span>
                    <span>Lần kết nối lại: <b>{subStatus.reconnect_count ?? 0}</b></span>
                    <span>Cuối cùng nhận: <b>{fmtSec(subStatus.seconds_since_event)}</b> trước</span>
                    <span style={{color:'var(--text-dim)'}}>
                      {subStatus.state === 'failed' && 'Đang thử kết nối lại sau 5s…'}
                      {subStatus.state === 'active' && subStatus.total_events === 0 && 'Chưa có sự kiện'}
                    </span>
                  </div>
                </div>
              );
            })()}

            <div style={{marginTop:'auto', paddingTop:'20px', display:'flex', gap:'10px'}}>
                <button onClick={onClose} disabled={loading || deepScanning} style={{flex:1, padding:'10px', background:'transparent', border:'1px solid var(--border)', color:'var(--text-secondary)', cursor:'pointer', fontWeight:'bold', borderRadius:'4px'}}>HỦY BỎ</button>
                <button onClick={handleSave} disabled={loading || deepScanning} style={{flex:1, padding:'10px', background:loading ? '#333' : 'var(--accent)', border:'none', color:'#000', fontWeight:'bold', cursor:loading ? 'wait' : 'pointer', borderRadius:'4px', boxShadow:'0 0 10px rgba(0,200,245,0.4)'}}>
                  {loading ? 'ĐANG LƯU...' : initialData ? 'CẬP NHẬT' : 'LƯU CAMERA'}
                </button>
            </div>
          </div>
          
          {/* ROI Canvas */}
          <div style={{flex:1, padding:'20px', display:'flex', flexDirection:'column'}}>
            <div style={{display:'flex', justifyContent:'space-between', alignItems:'center', marginBottom:'10px'}}>
               <div style={{fontSize:'12px', color:'var(--text-secondary)', display:'flex', alignItems:'center'}}>
                 <Activity size={14} style={{marginRight:6, color:'var(--danger)'}}/> Trình Vẽ Vùng Nhận Diện (AI ROI Canvas)
               </div>
            </div>
            <div style={{flex:1, background:'#06090e', border:'1px solid var(--border)', position:'relative', borderRadius:'4px', overflow:'hidden', boxShadow:'inset 0 0 20px rgba(0,0,0,0.8)'}}>
               <div style={{position:'absolute', top:0, left:0, right:0, bottom:0, background:'radial-gradient(ellipse at center, #1b283d 0%, #06090e 100%)', opacity:0.3}}></div>
               <div style={{position:'absolute', top:0, left:0, right:0, bottom:0, backgroundImage:'linear-gradient(rgba(0, 200, 245, 0.05) 1px, transparent 1px), linear-gradient(90deg, rgba(0, 200, 245, 0.05) 1px, transparent 1px)', backgroundSize:'40px 40px'}}></div>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
};

export default CameraConfigModal;
