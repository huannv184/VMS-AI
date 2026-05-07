import React, { useState, useEffect, useCallback } from 'react';
import { MapPin, Building2, ChevronRight, Plus, Pencil, Trash2, Camera, X, Check } from 'lucide-react';
import apiClient from '../api/apiClient';
import useVmsStore from '../store/useVmsStore';

const EMPTY_FORM = { name: '', address: '', parent_site_id: null };

const SiteManager = () => {
  const [sites, setSites] = useState([]);
  const [loading, setLoading] = useState(true);
  const cameras = useVmsStore(state => state.cameras);
  const [selectedSite, setSelectedSite] = useState(null);
  const [modal, setModal] = useState(null); // { mode: 'add'|'edit'|'add-child', site?, parentId? }
  const [form, setForm] = useState(EMPTY_FORM);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState(null);

  const fetchSites = useCallback(async () => {
    setLoading(true);
    try {
      const res = await apiClient.getSites();
      if (res.success) setSites(res.data?.sites || []);
    } catch (err) {
      console.error('Failed to fetch sites:', err);
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => { fetchSites(); }, [fetchSites]);

  const getSubSites = (parentId) => sites.filter(s => s.parent_site_id === parentId);

  const openAdd = () => {
    setForm(EMPTY_FORM);
    setError(null);
    setModal({ mode: 'add' });
  };

  const openAddChild = (parentId) => {
    setForm({ ...EMPTY_FORM, parent_site_id: parentId });
    setError(null);
    setModal({ mode: 'add-child', parentId });
  };

  const openEdit = (site) => {
    setForm({ name: site.name, address: site.address || '', parent_site_id: site.parent_site_id ?? null });
    setError(null);
    setModal({ mode: 'edit', site });
  };

  const handleSave = async () => {
    if (!form.name.trim()) { setError('Tên vị trí không được để trống'); return; }
    setSaving(true);
    setError(null);
    try {
      let res;
      if (modal.mode === 'edit') {
        res = await apiClient.updateSite(modal.site.id, form);
      } else {
        res = await apiClient.addSite(form);
      }
      if (res.success) {
        setModal(null);
        await fetchSites();
      } else {
        setError(res.error || 'Lỗi không xác định');
      }
    } catch (err) {
      setError(err.message);
    } finally {
      setSaving(false);
    }
  };

  const handleDelete = async (siteId) => {
    if (!window.confirm('Xác nhận xóa vị trí này?')) return;
    const res = await apiClient.deleteSite(siteId);
    if (res.success) {
      if (selectedSite === siteId) setSelectedSite(null);
      await fetchSites();
    } else {
      alert(res.error || 'Xóa thất bại');
    }
  };

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
            display:'flex', alignItems:'center', padding:'10px 15px',
            background: isExpanded ? 'rgba(0,200,245,0.08)' : 'rgba(255,255,255,0.02)',
            border:'1px solid var(--border)', borderRadius:'6px', marginBottom:'5px',
            cursor:'pointer', transition:'all 0.2s'
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
            {siteCameras.map(cam => (
              <div key={cam.id} style={{ marginLeft:30, padding:'8px 15px', fontSize:12, color:'var(--text-secondary)', display:'flex', alignItems:'center', gap:10 }}>
                <Camera size={12} /> {cam.name}
                <span className="config-badge active" style={{ fontSize:8 }}>{cam.status}</span>
              </div>
            ))}
            {subSites.map(sub => (
              <SiteItem key={sub.id} site={sub} level={level + 1} />
            ))}
            <div
              onClick={(e) => { e.stopPropagation(); openAddChild(site.id); }}
              style={{ marginLeft:30, padding:'8px 15px', fontSize:11, color:'var(--accent)', cursor:'pointer', display:'flex', alignItems:'center', gap:5 }}
            >
              <Plus size={12} /> Thêm vị trí con
            </div>
          </div>
        )}
      </div>
    );
  };

  const currentSite = sites.find(s => s.id === selectedSite);
  const siteCamerasOnline = currentSite
    ? cameras.filter(c => c.site_id === currentSite.id && c.status === 'online').length
    : 0;
  const siteCamerasTotal = currentSite
    ? cameras.filter(c => c.site_id === currentSite.id).length
    : 0;

  return (
    <div style={{ padding: 20 }}>
      <div style={{ display:'flex', justifyContent:'space-between', alignItems:'center', marginBottom:20 }}>
        <div style={{ fontFamily:'Rajdhani', fontWeight:700, fontSize:20, letterSpacing:1, color:'var(--accent)' }}>
          QUẢN LÝ VỊ TRÍ & CHI NHÁNH (MULTI-SITE)
        </div>
        <button className="pb-btn" style={{ background:'var(--accent)', color:'#000', fontWeight:'bold' }} onClick={openAdd}>
          <Plus size={14} style={{marginRight:6}}/> THÊM SITE MỚI
        </button>
      </div>

      {loading ? (
        <div style={{ textAlign:'center', padding:40, color:'var(--text-dim)' }}>Đang tải...</div>
      ) : (
        <div style={{ display:'grid', gridTemplateColumns:'1fr 1fr', gap:30 }}>
          {/* Left: Site Tree */}
          <div className="glass" style={{ padding:20, borderRadius:12 }}>
            <div style={{ fontSize:12, fontWeight:700, marginBottom:15, color:'var(--text-dim)', letterSpacing:1 }}>HỆ THỐNG PHÂN CẤP (TREE VIEW)</div>
            {sites.filter(s => s.parent_site_id === -1 || !s.parent_site_id).map(root => (
              <SiteItem key={root.id} site={root} />
            ))}
            {sites.length === 0 && (
              <div style={{ textAlign:'center', padding:30, color:'var(--text-dim)', fontSize:12 }}>
                Chưa có vị trí. Bấm "THÊM SITE MỚI".
              </div>
            )}
          </div>

          {/* Right: Site Details */}
          <div className="glass" style={{ padding:20, borderRadius:12, display:'flex', flexDirection:'column', gap:20 }}>
            <div style={{ fontSize:12, fontWeight:700, color:'var(--text-dim)', letterSpacing:1 }}>THÔNG TIN CHI TIẾT VỊ TRÍ</div>
            {currentSite ? (
              <div>
                <div style={{ fontSize:22, fontWeight:700, color:'var(--accent3)' }}>{currentSite.name}</div>
                <div style={{ fontSize:13, color:'var(--text-secondary)', marginTop:5 }}>📍 {currentSite.address || '—'}</div>

                <div style={{ marginTop:30 }}>
                  <div style={{ fontSize:11, fontWeight:700, color:'var(--text-dim)', marginBottom:10 }}>THỐNG KÊ TẠI SITE</div>
                  <div style={{ display:'grid', gridTemplateColumns:'1fr 1fr', gap:10 }}>
                    <div style={{ padding:15, background:'rgba(255,255,255,0.03)', borderRadius:8, border:'1px solid var(--border)' }}>
                      <div style={{ fontSize:24, fontFamily:'Share Tech Mono', color:'var(--accent)' }}>{siteCamerasOnline}</div>
                      <div style={{ fontSize:9, color:'var(--text-dim)' }}>CAMERA TRỰC TUYẾN / {siteCamerasTotal}</div>
                    </div>
                    <div style={{ padding:15, background:'rgba(255,255,255,0.03)', borderRadius:8, border:'1px solid var(--border)' }}>
                      <div style={{ fontSize:24, fontFamily:'Share Tech Mono', color:'var(--warn)' }}>
                        {getSubSites(currentSite.id).length}
                      </div>
                      <div style={{ fontSize:9, color:'var(--text-dim)' }}>CHI NHÁNH CON</div>
                    </div>
                  </div>
                </div>

                <div style={{ display:'flex', gap:10, marginTop:30 }}>
                  <button className="grid-btn" style={{ flex:1 }} onClick={() => openEdit(currentSite)}>
                    <Pencil size={14} style={{marginRight:6}}/> Chỉnh sửa
                  </button>
                  <button className="grid-btn danger" style={{ flex:1, color:'var(--danger)' }} onClick={() => handleDelete(currentSite.id)}>
                    <Trash2 size={14} style={{marginRight:6}}/> Xóa Vị trí
                  </button>
                </div>
              </div>
            ) : (
              <div style={{ flex:1, display:'flex', alignItems:'center', justifyContent:'center', border:'1px dashed var(--border)', borderRadius:8, color:'var(--text-dim)', fontSize:12 }}>
                Chọn một vị trí bên trái để xem chi tiết
              </div>
            )}
          </div>
        </div>
      )}

      {/* Modal Add/Edit */}
      {modal && (
        <div style={{ position:'fixed', inset:0, background:'rgba(0,0,0,0.6)', display:'flex', alignItems:'center', justifyContent:'center', zIndex:1000 }}>
          <div style={{ background:'var(--bg-card)', border:'1px solid var(--border)', borderRadius:10, padding:24, width:400, display:'flex', flexDirection:'column', gap:14 }}>
            <div style={{ display:'flex', justifyContent:'space-between', alignItems:'center' }}>
              <span style={{ fontWeight:700, fontSize:15, color:'var(--accent)' }}>
                {modal.mode === 'edit' ? 'Chỉnh sửa vị trí' : 'Thêm vị trí mới'}
              </span>
              <X size={16} style={{ cursor:'pointer' }} onClick={() => setModal(null)} />
            </div>
            <div style={{ display:'flex', flexDirection:'column', gap:10 }}>
              <input
                className="pb-input"
                placeholder="Tên vị trí *"
                value={form.name}
                onChange={e => setForm(f => ({ ...f, name: e.target.value }))}
              />
              <input
                className="pb-input"
                placeholder="Địa chỉ"
                value={form.address}
                onChange={e => setForm(f => ({ ...f, address: e.target.value }))}
              />
            </div>
            {error && <div style={{ fontSize:12, color:'var(--danger)' }}>{error}</div>}
            <div style={{ display:'flex', gap:10 }}>
              <button className="grid-btn" style={{ flex:1 }} onClick={() => setModal(null)}>Hủy</button>
              <button className="pb-btn" style={{ flex:1, background:'var(--accent)', color:'#000' }} onClick={handleSave} disabled={saving}>
                <Check size={14} style={{marginRight:4}}/> {saving ? 'Đang lưu...' : 'Lưu'}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
};

export default SiteManager;
