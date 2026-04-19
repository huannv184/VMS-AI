import React, { useState, useEffect } from 'react';
import { MapPin, Building2, Layout, ChevronRight, Plus, Pencil, Trash2, Camera } from 'lucide-react';
import apiClient from '../api/apiClient';
import useVmsStore from '../store/useVmsStore';

const SiteManager = () => {
  const [sites, setSites] = useState([]);
  const [loading, setLoading] = useState(true);
  const cameras = useVmsStore(state => state.cameras);
  const [selectedSite, setSelectedSite] = useState(null);

  useEffect(() => {
    fetchSites();
  }, []);

  const fetchSites = async () => {
    setLoading(true);
    try {
      const res = await apiClient.getSites();
      if (res.success) setSites(res.data?.sites || []);
    } catch (err) {
      console.error("Failed to fetch sites:", err);
    } finally {
      setLoading(false);
    }
  };

  const getSubSites = (parentId) => sites.filter(s => s.parent_site_id === parentId);

  const SiteItem = ({ site, level = 0 }) => {
    const subSites = getSubSites(site.id);
    const siteCameras = cameras.filter(c => c.site_id === site.id);
    const isExpanded = selectedSite === site.id;

    return (
      <div style={{ marginLeft: level * 20 }}>
        <div 
          className={`site-row ${isExpanded ? 'active' : ''}`}
          onClick={() => setSelectedSite(isExpanded ? null : site.id)}
          style={{ 
            display:'flex', 
            alignItems:'center', 
            padding:'10px 15px', 
            background: isExpanded ? 'rgba(0,200,245,0.08)' : 'rgba(255,255,255,0.02)',
            border:'1px solid var(--border)',
            borderRadius:'6px',
            marginBottom:'5px',
            cursor:'pointer',
            transition:'all 0.2s'
          }}
        >
          {level === 0 ? <MapPin size={16} color="var(--accent)" /> : <Building2 size={14} color="var(--accent3)" />}
          <span style={{ flex:1, marginLeft:10, fontSize:13, fontWeight:600 }}>{site.name}</span>
          <div style={{ fontSize:10, color:'var(--text-dim)', marginRight:15 }}>
            {siteCameras.length} CAMs · {subSites.length} Sub-sites
          </div>
          <ChevronRight size={14} style={{ transform: isExpanded ? 'rotate(90deg)' : 'none', transition:'transform 0.2s' }} />
        </div>

        {isExpanded && (
          <div style={{ marginBottom:10 }}>
            {/* Render Cameras in this site */}
            {siteCameras.map(cam => (
              <div key={cam.id} style={{ marginLeft: 30, padding:'8px 15px', fontSize:12, color:'var(--text-secondary)', display:'flex', alignItems:'center', gap:10 }}>
                <Camera size={12} /> {cam.name}
                <span className="config-badge active" style={{ fontSize:8 }}>{cam.status}</span>
              </div>
            ))}
            {/* Render Sub Sites */}
            {subSites.map(sub => (
              <SiteItem key={sub.id} site={sub} level={level + 1} />
            ))}
            <div style={{ marginLeft: 30, padding:'8px 15px', fontSize:11, color:'var(--accent)', cursor:'pointer', display:'flex', alignItems:'center', gap:5 }}>
               <Plus size={12} /> Thêm vị trí con
            </div>
          </div>
        )}
      </div>
    );
  };

  return (
    <div style={{ padding: 20 }}>
      <div style={{ display:'flex', justifyContent:'space-between', alignItems:'center', marginBottom:20 }}>
        <div style={{ fontFamily:'Rajdhani', fontWeight:700, fontSize:20, letterSpacing:1, color:'var(--accent)' }}>QUẢN LÝ VỊ TRÍ & CHI NHÁNH (MULTI-SITE)</div>
        <button className="pb-btn" style={{ background:'var(--accent)', color:'#000', fontWeight:'bold' }}>
           <Plus size={14} style={{marginRight:6}}/> THÊM SITE MỚI
        </button>
      </div>

      <div style={{ display:'grid', gridTemplateColumns:'1fr 1fr', gap:30 }}>
        {/* Left: Site Tree */}
        <div className="glass" style={{ padding:20, borderRadius:12 }}>
          <div style={{ fontSize:12, fontWeight:700, marginBottom:15, color:'var(--text-dim)', letterSpacing:1 }}>HỆ THỐNG PHÂN CẤP (TREE VIEW)</div>
          {sites.filter(s => s.parent_site_id === -1 || !s.parent_site_id).map(root => (
            <SiteItem key={root.id} site={root} />
          ))}
        </div>

        {/* Right: Site Details / Map Preview */}
        <div className="glass" style={{ padding:20, borderRadius:12, display:'flex', flexDirection:'column', gap:20 }}>
           <div style={{ fontSize:12, fontWeight:700, color:'var(--text-dim)', letterSpacing:1 }}>THÔNG TIN CHI TIẾT VỊ TRÍ</div>
           {selectedSite ? (
             <div>
                <div style={{ fontSize:22, fontWeight:700, color:'var(--accent3)' }}>{sites.find(s => s.id === selectedSite)?.name}</div>
                <div style={{ fontSize:13, color:'var(--text-secondary)', marginTop:5 }}>📍 {sites.find(s => s.id === selectedSite)?.address}</div>
                
                <div style={{ marginTop:30 }}>
                   <div style={{ fontSize:11, fontWeight:700, color:'var(--text-dim)', marginBottom:10 }}>THỐNG KÊ AI TẠI SITE</div>
                   <div style={{ display:'grid', gridTemplateColumns:'1fr 1fr', gap:10 }}>
                      <div style={{ padding:15, background:'rgba(255,255,255,0.03)', borderRadius:8, border:'1px solid var(--border)' }}>
                         <div style={{ fontSize:24, fontFamily:'Share Tech Mono', color:'var(--accent)' }}>12</div>
                         <div style={{ fontSize:9, color:'var(--text-dim)' }}>CAMERA TRỰC TUYẾN</div>
                      </div>
                      <div style={{ padding:15, background:'rgba(255,255,255,0.03)', borderRadius:8, border:'1px solid var(--border)' }}>
                         <div style={{ fontSize:24, fontFamily:'Share Tech Mono', color:'var(--warn)' }}>08</div>
                         <div style={{ fontSize:9, color:'var(--text-dim)' }}>CẢNH BÁO / 24H</div>
                      </div>
                   </div>
                </div>

                <div style={{ display:'flex', gap:10, marginTop:30 }}>
                   <button className="grid-btn" style={{ flex:1 }}><Pencil size={14} style={{marginRight:6}}/> Chỉnh sửa</button>
                   <button className="grid-btn danger" style={{ flex:1, color:'var(--danger)' }}><Trash2 size={14} style={{marginRight:6}}/> Xóa Vị trí</button>
                </div>
             </div>
           ) : (
             <div style={{ flex:1, display:'flex', alignItems:'center', justifyContent:'center', border:'1px dashed var(--border)', borderRadius:8, color:'var(--text-dim)', fontSize:12 }}>
                Chọn một vị trí bên trái để xem chi tiết
             </div>
           )}
        </div>
      </div>
    </div>
  );
};

export default SiteManager;
