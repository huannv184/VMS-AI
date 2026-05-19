import React, { useState, useEffect, useCallback, useRef } from 'react';
import useVmsStore from '../store/useVmsStore';
import apiClient from '../api/apiClient';

const ReIDView = () => {
  const cameras = useVmsStore(s => s.cameras);
  const [gallery, setGallery] = useState([]);
  const [stats, setStats] = useState({});
  // FE-3 FIX (2026-05-09): match_threshold default kept loosely in sync with
  // backend Fix-B (0.72) so the slider shows the right value before
  // fetchConfig completes. fetchConfig still overrides with the actual
  // server value once the request lands. The other defaults
  // (gallery_ttl_sec, max_gallery_size) match the engine header.
  const [config, setConfig] = useState({ enabled: true, match_threshold: 0.72, gallery_ttl_sec: 1800, max_gallery_size: 500 });
  // FE-2 FIX: surface backend's BUG-REID-DEAD-PIPELINE producer_wired flag
  // so the empty-gallery message reflects "feature dormant" instead of
  // "scene quiet". Default true so the UX doesn't flash the warning during
  // the first fetch race.
  const [producerWired, setProducerWired] = useState(true);
  const [selectedPerson, setSelectedPerson] = useState(null);
  const [trail, setTrail] = useState([]);
  const [searchResults, setSearchResults] = useState(null);
  const [showConfig, setShowConfig] = useState(false);
  const fileInputRef = useRef(null);

  // Fetch gallery
  const fetchGallery = useCallback(async () => {
    try {
      const res = await apiClient.getReIDGallery?.();
      if (res?.success && res.data?.gallery) {
        setGallery(res.data.gallery);
        setStats(res.data.statistics || {});
        // Backend response includes producer_wired (BUG-REID-DEAD-PIPELINE
        // 2026-05-08). Defaults to true if absent so older backends don't
        // trigger the warning.
        if (typeof res.data.producer_wired === 'boolean') {
          setProducerWired(res.data.producer_wired);
        }
      }
    } catch (e) {
      // Background poll — don't toast, but surface in console so a dev
      // debugging "gallery seems frozen" has a breadcrumb.
      console.error('[ReIDView] gallery fetch failed:', e);
    }
  }, []);

  // Fetch config
  const fetchConfig = useCallback(async () => {
    try {
      const res = await apiClient.getReIDConfig?.();
      if (res?.success) setConfig(res.data || {});
    } catch (e) {
      console.error('[ReIDView] config fetch failed:', e);
    }
  }, []);

  useEffect(() => {
    fetchGallery();
    fetchConfig();
    const interval = setInterval(fetchGallery, 5000);
    return () => clearInterval(interval);
  }, [fetchGallery, fetchConfig]);

  // Load trail
  const loadTrail = useCallback(async (globalId) => {
    setSelectedPerson(globalId);
    try {
      const res = await apiClient.getReIDTrail?.(globalId);
      if (res?.success) setTrail(res.data?.trail || []);
    } catch (e) {
      console.error('[ReIDView] trail load failed:', e);
    }
  }, []);

  // Search by image
  const handleSearchUpload = async (e) => {
    const file = e.target.files?.[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = async (ev) => {
      try {
        const base64 = ev.target.result;
        const res = await apiClient.searchReID?.(base64);
        if (res?.success) setSearchResults(res.data?.results || []);
      } catch (err) {
        console.error('[ReIDView] image search failed:', err);
        // User-initiated upload — surface failure so they don't keep
        // re-uploading hoping for results.
        window.alert(`Tìm kiếm Re-ID thất bại: ${err?.message || err}`);
      }
    };
    reader.readAsDataURL(file);
  };

  // Update config
  const updateConfig = async (key, value) => {
    const newConfig = { ...config, [key]: value };
    setConfig(newConfig);
    try {
      await apiClient.updateReIDConfig?.(newConfig);
    } catch (err) {
      console.error('[ReIDView] config update failed:', err);
      // Revert optimistic UI so user sees their change didn't stick.
      setConfig(config);
      window.alert(`Cập nhật config Re-ID thất bại: ${err?.message || err}`);
    }
  };

  // Clear gallery
  const clearGallery = async () => {
    if (!window.confirm('Xóa toàn bộ gallery Re-ID?')) return;
    try {
      await apiClient.clearReIDGallery?.();
      setGallery([]);
      setStats({});
      setSelectedPerson(null);
      setTrail([]);
    } catch (err) {
      console.error('[ReIDView] clear gallery failed:', err);
      // Critical destructive action — operator clicked, must know if it
      // succeeded or not. Pre-fix the UI cleared locally even on 403/500
      // and gallery reappeared on next poll, creating "what happened?" UX.
      window.alert(`Xóa gallery thất bại: ${err?.message || err}`);
    }
  };

  const getCameraName = (camId) => {
    const cam = cameras.find(c => c.id === camId);
    return cam ? cam.name : `Camera ${camId}`;
  };

  const formatTime = (epoch) => {
    if (!epoch) return '--';
    return new Date(epoch * 1000).toLocaleString('vi-VN');
  };

  return (
    <div className="view-panel active" id="view-reid" style={{ display: 'flex', flexDirection: 'column', height: '100%', overflow: 'hidden' }}>
      {/* Header */}
      <div className="grid-toolbar" style={{ justifyContent: 'space-between' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5" style={{ width: 14, height: 14, color: 'var(--accent)' }}>
            <circle cx="5" cy="4" r="2"/><path d="M1 10c0-2 2-3 4-3s4 1 4 3"/><circle cx="10" cy="5" r="1.5"/><path d="M10 8c1.5 0 3 .7 3 2"/>
          </svg>
          <span style={{ fontFamily: "'Rajdhani', sans-serif", fontWeight: 700, fontSize: 12, letterSpacing: 2, color: 'var(--accent)' }}>RE-ID TRACKING</span>
          <div className="toolbar-sep2"></div>

          {/* Stats */}
          <div style={{ display: 'flex', gap: 12, fontSize: 10, color: 'var(--text-dim)' }}>
            <span>Gallery: <b style={{ color: 'var(--accent)' }}>{stats.gallery_size || 0}</b></span>
            <span>Matched: <b style={{ color: '#4CAF50' }}>{stats.total_matched || 0}</b></span>
            <span>New: <b style={{ color: '#FF9800' }}>{stats.total_new || 0}</b></span>
            <span>Processed: <b>{stats.total_processed || 0}</b></span>
            <span style={{ color: stats.model_loaded ? '#4CAF50' : '#f44' }}>{stats.model_loaded ? '● DNN Model' : '● Histogram Fallback'}</span>
          </div>
        </div>

        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
          <div className="grid-btn" onClick={() => fileInputRef.current?.click()} title="Search by Image">
            <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5" style={{ width: 12, height: 12 }}>
              <circle cx="6" cy="6" r="4"/><path d="M9 9l3 3"/>
            </svg>
            SEARCH
          </div>
          <input ref={fileInputRef} type="file" accept="image/*" style={{ display: 'none' }} onChange={handleSearchUpload} />

          <div className="grid-btn" onClick={() => setShowConfig(!showConfig)} style={{ color: showConfig ? 'var(--accent)' : undefined }}>
            <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5" style={{ width: 12, height: 12 }}>
              <circle cx="7" cy="7" r="2"/><path d="M7 1v2M7 11v2M1 7h2M11 7h2M2.8 2.8l1.4 1.4M9.8 9.8l1.4 1.4M11.2 2.8l-1.4 1.4M4.2 9.8l-1.4 1.4"/>
            </svg>
            CONFIG
          </div>

          <div className="grid-btn" onClick={clearGallery} style={{ color: 'var(--danger)' }}>
            CLEAR
          </div>

          <div className="grid-btn" onClick={fetchGallery}>
            REFRESH
          </div>
        </div>
      </div>

      {/* Config Panel */}
      {showConfig && (
        <div style={{ padding: '10px 16px', borderBottom: '1px solid rgba(255,255,255,0.06)', display: 'flex', gap: 24, alignItems: 'center', fontSize: 10, flexShrink: 0, background: 'rgba(0,0,0,0.2)' }}>
          <label style={{ display: 'flex', alignItems: 'center', gap: 6, color: 'var(--text-dim)' }}>
            <input type="checkbox" checked={config.enabled} onChange={e => updateConfig('enabled', e.target.checked)} />
            Enabled
          </label>
          <label style={{ display: 'flex', alignItems: 'center', gap: 6, color: 'var(--text-dim)' }}>
            Threshold:
            <input type="range" min={0.3} max={0.95} step={0.05} value={config.match_threshold}
                   onChange={e => updateConfig('match_threshold', parseFloat(e.target.value))}
                   style={{ width: 80 }} />
            <b style={{ color: '#fff' }}>{config.match_threshold?.toFixed(2)}</b>
          </label>
          <label style={{ display: 'flex', alignItems: 'center', gap: 6, color: 'var(--text-dim)' }}>
            TTL:
            <input type="number" value={config.gallery_ttl_sec || 1800}
                   onChange={e => updateConfig('gallery_ttl_sec', parseInt(e.target.value) || 1800)}
                   style={{ width: 60, background: 'rgba(255,255,255,0.05)', border: '1px solid rgba(255,255,255,0.1)', borderRadius: 3, color: '#fff', textAlign: 'center', fontSize: 10, padding: '2px 4px' }} />
            sec
          </label>
          <label style={{ display: 'flex', alignItems: 'center', gap: 6, color: 'var(--text-dim)' }}>
            Max Gallery:
            <input type="number" value={config.max_gallery_size || 500}
                   onChange={e => updateConfig('max_gallery_size', parseInt(e.target.value) || 500)}
                   style={{ width: 60, background: 'rgba(255,255,255,0.05)', border: '1px solid rgba(255,255,255,0.1)', borderRadius: 3, color: '#fff', textAlign: 'center', fontSize: 10, padding: '2px 4px' }} />
          </label>
        </div>
      )}

      {/* Main Content */}
      <div style={{ display: 'flex', flex: 1, overflow: 'hidden' }}>
        {/* Gallery Grid */}
        <div style={{ flex: 1, overflowY: 'auto', padding: 12 }}>
          {searchResults && (
            <div style={{ marginBottom: 16, padding: 12, background: 'rgba(0,200,245,0.05)', borderRadius: 8, border: '1px solid rgba(0,200,245,0.15)' }}>
              <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 8 }}>
                <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--accent)', letterSpacing: 1 }}>SEARCH RESULTS ({searchResults.length})</span>
                <span style={{ cursor: 'pointer', fontSize: 10, color: 'var(--text-dim)' }} onClick={() => setSearchResults(null)}>✕ Close</span>
              </div>
              <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
                {searchResults.map((r, i) => (
                  <div key={i} onClick={() => loadTrail(r.global_id)}
                       style={{ cursor: 'pointer', padding: 8, background: 'rgba(0,0,0,0.3)', borderRadius: 6, border: '1px solid rgba(255,255,255,0.06)', textAlign: 'center' }}>
                    {r.thumbnail_path && <img src={`/${r.thumbnail_path}`} alt="" style={{ width: 48, height: 80, objectFit: 'cover', borderRadius: 4 }} />}
                    <div style={{ fontSize: 9, color: '#fff', marginTop: 4 }}>ID: {r.global_id}</div>
                    <div style={{ fontSize: 8, color: 'var(--accent)' }}>{(r.score * 100).toFixed(0)}% match</div>
                  </div>
                ))}
              </div>
            </div>
          )}

          <div style={{ fontSize: 10, fontWeight: 700, letterSpacing: 1, color: 'var(--text-dim)', marginBottom: 10 }}>
            ACTIVE GALLERY ({gallery.length})
          </div>

          {!producerWired && (
            <div style={{ marginBottom: 16, padding: 12, background: 'rgba(255, 152, 0, 0.08)', borderRadius: 8, border: '1px solid rgba(255, 152, 0, 0.3)', fontSize: 11, color: '#FF9800' }}>
              <strong>⚠ Re-ID detection pipeline not connected.</strong> The cross-camera tracking
              feature is dormant — no producer is feeding the gallery. The backend reports this
              as <code>producer_wired: false</code> (BUG-REID-DEAD-PIPELINE). Search-by-image
              still works against the empty gallery; live tracking will appear once the
              inference pipeline wires <code>processDetection()</code>.
            </div>
          )}
          {gallery.length === 0 ? (
            <div style={{ textAlign: 'center', padding: 60, color: 'var(--text-dim)', fontSize: 12 }}>
              <div style={{ fontSize: 40, marginBottom: 12, opacity: 0.3 }}>👤</div>
              {producerWired ? (
                <>
                  No persons in Re-ID gallery yet.<br/>
                  <span style={{ fontSize: 10 }}>Persons will appear here as AI detects them across cameras.</span>
                </>
              ) : (
                <>
                  Re-ID gallery is empty because the detection pipeline is not yet active.<br/>
                  <span style={{ fontSize: 10 }}>See the warning banner above.</span>
                </>
              )}
            </div>
          ) : (
            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(100px, 1fr))', gap: 8 }}>
              {gallery.map(entry => (
                <div key={entry.global_id}
                     onClick={() => loadTrail(entry.global_id)}
                     style={{
                       cursor: 'pointer',
                       background: selectedPerson === entry.global_id ? 'rgba(0,200,245,0.1)' : 'rgba(255,255,255,0.02)',
                       border: selectedPerson === entry.global_id ? '1px solid rgba(0,200,245,0.3)' : '1px solid rgba(255,255,255,0.06)',
                       borderRadius: 8,
                       padding: 8,
                       textAlign: 'center',
                       transition: 'all 0.2s'
                     }}>
                  <div style={{ width: '100%', aspectRatio: '3/5', borderRadius: 6, overflow: 'hidden', marginBottom: 6, background: '#1a1a2e' }}>
                    {entry.thumbnail_path ? (
                      <img src={`/${entry.thumbnail_path}`} alt=""
                           style={{ width: '100%', height: '100%', objectFit: 'cover' }}
                           onError={e => { e.target.style.display = 'none'; }} />
                    ) : (
                      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100%', fontSize: 24, opacity: 0.2 }}>👤</div>
                    )}
                  </div>
                  <div style={{ fontSize: 10, fontWeight: 700, color: 'var(--accent)' }}>ID: {entry.global_id}</div>
                  <div style={{ fontSize: 8, color: 'var(--text-dim)' }}>{getCameraName(entry.camera_id)}</div>
                  <div style={{ fontSize: 7, color: 'var(--text-dim)', marginTop: 2 }}>
                    {formatTime(entry.last_seen)}
                  </div>
                </div>
              ))}
            </div>
          )}
        </div>

        {/* Trail Panel */}
        {selectedPerson && (
          <div style={{ width: 300, borderLeft: '1px solid rgba(255,255,255,0.06)', overflowY: 'auto', padding: 12, flexShrink: 0, background: 'rgba(0,0,0,0.15)' }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
              <span style={{ fontSize: 12, fontWeight: 700, color: 'var(--accent)', letterSpacing: 1 }}>
                TRAIL · Person #{selectedPerson}
              </span>
              <span style={{ cursor: 'pointer', color: 'var(--text-dim)', fontSize: 10 }} onClick={() => { setSelectedPerson(null); setTrail([]); }}>✕</span>
            </div>

            <div style={{ fontSize: 10, color: 'var(--text-dim)', marginBottom: 12 }}>
              Cameras visited: <b style={{ color: '#fff' }}>{trail.length}</b>
            </div>

            {/* Timeline */}
            <div style={{ position: 'relative', paddingLeft: 20 }}>
              {/* Vertical line */}
              <div style={{ position: 'absolute', left: 7, top: 0, bottom: 0, width: 2, background: 'linear-gradient(to bottom, var(--accent), rgba(0,200,245,0.1))' }}></div>

              {trail.map((point, i) => (
                <div key={i} style={{ position: 'relative', marginBottom: 16, paddingLeft: 16 }}>
                  {/* Dot */}
                  <div style={{
                    position: 'absolute', left: -17, top: 4,
                    width: 10, height: 10, borderRadius: '50%',
                    background: i === trail.length - 1 ? 'var(--accent)' : 'rgba(0,200,245,0.4)',
                    border: '2px solid var(--bg)',
                    boxShadow: i === trail.length - 1 ? '0 0 8px var(--accent)' : 'none'
                  }}></div>

                  <div style={{ background: 'rgba(255,255,255,0.03)', borderRadius: 6, padding: 10, border: '1px solid rgba(255,255,255,0.06)' }}>
                    <div style={{ fontSize: 11, fontWeight: 700, color: '#fff', marginBottom: 4 }}>
                      📹 {getCameraName(point.camera_id)}
                    </div>
                    {point.thumbnail_path && (
                      <img src={`/${point.thumbnail_path}`} alt=""
                           style={{ width: '100%', height: 80, objectFit: 'cover', borderRadius: 4, marginBottom: 4 }}
                           onError={e => { e.target.style.display = 'none'; }} />
                    )}
                    <div style={{ fontSize: 8, color: 'var(--text-dim)' }}>
                      Enter: {formatTime(point.enter_time)}
                    </div>
                    <div style={{ fontSize: 8, color: 'var(--text-dim)' }}>
                      Exit: {formatTime(point.exit_time)}
                    </div>
                  </div>
                </div>
              ))}

              {trail.length === 0 && (
                <div style={{ color: 'var(--text-dim)', fontSize: 10, padding: 16 }}>
                  No trail data available
                </div>
              )}
            </div>
          </div>
        )}
      </div>
    </div>
  );
};

export default ReIDView;
