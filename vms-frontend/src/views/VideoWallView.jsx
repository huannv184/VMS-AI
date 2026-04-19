import React, { useState, useEffect, useMemo, useCallback, useRef } from 'react';
import useVmsStore from '../store/useVmsStore';
import CameraFeed from '../components/CameraFeed';
import apiClient from '../api/apiClient';

const GRID_PRESETS = [
  { label: '1×1', cols: 1, rows: 1 },
  { label: '2×2', cols: 2, rows: 2 },
  { label: '3×3', cols: 3, rows: 3 },
  { label: '4×4', cols: 4, rows: 4 },
  { label: '5×3', cols: 5, rows: 3 },
  { label: '4×3', cols: 4, rows: 3 },
  { label: '6×4', cols: 6, rows: 4 },
  { label: '8×8', cols: 8, rows: 8 },
];

const VideoWallView = () => {
  const cameras = useVmsStore((s) => s.cameras);
  const [gridCols, setGridCols] = useState(4);
  const [gridRows, setGridRows] = useState(4);
  const [cells, setCells] = useState({}); // { "0": camera_id, "1": camera_id, ... }
  const [layouts, setLayouts] = useState([]);
  const [layoutName, setLayoutName] = useState('');
  const [activeLayoutId, setActiveLayoutId] = useState(null);
  const [isFullscreen, setIsFullscreen] = useState(false);
  const [showLayoutPanel, setShowLayoutPanel] = useState(false);
  const [dragOverCell, setDragOverCell] = useState(null);
  const [zoomCell, setZoomCell] = useState(null);
  const wallRef = useRef(null);

  const totalCells = gridCols * gridRows;

  // Load layouts
  useEffect(() => {
    apiClient.getVideoWallLayouts?.().then(res => {
      if (res?.success) setLayouts(res.data?.layouts || []);
    }).catch(() => {});
  }, []);

  // Auto-fill cells with cameras if no layout loaded
  useEffect(() => {
    if (Object.keys(cells).length === 0 && cameras.length > 0) {
      const auto = {};
      cameras.slice(0, totalCells).forEach((cam, i) => {
        auto[String(i)] = cam.id;
      });
      setCells(auto);
    }
  }, [cameras, totalCells]);

  // Fullscreen toggle
  const toggleFullscreen = useCallback(() => {
    if (!document.fullscreenElement) {
      wallRef.current?.requestFullscreen?.();
      setIsFullscreen(true);
    } else {
      document.exitFullscreen?.();
      setIsFullscreen(false);
    }
  }, []);

  useEffect(() => {
    const onChange = () => setIsFullscreen(!!document.fullscreenElement);
    document.addEventListener('fullscreenchange', onChange);
    return () => document.removeEventListener('fullscreenchange', onChange);
  }, []);

  // Layout actions
  const saveLayout = async () => {
    const name = layoutName.trim() || `Layout ${new Date().toLocaleString('vi-VN')}`;
    const cellsArray = Object.entries(cells).map(([idx, cam_id]) => ({
      cell_index: parseInt(idx), camera_id: cam_id
    }));

    try {
      if (activeLayoutId) {
        await apiClient.updateVideoWallLayout?.(activeLayoutId, {
          name, grid_cols: gridCols, grid_rows: gridRows, cells: cellsArray
        });
      } else {
        const res = await apiClient.createVideoWallLayout?.({
          name, grid_cols: gridCols, grid_rows: gridRows, cells: cellsArray
        });
        if (res?.success && res.data?.id) setActiveLayoutId(res.data.id);
      }
      const updated = await apiClient.getVideoWallLayouts?.();
      if (updated?.success) setLayouts(updated.data?.layouts || []);
    } catch (e) { console.error('Save layout failed', e); }
  };

  const loadLayout = (layout) => {
    setGridCols(layout.grid_cols);
    setGridRows(layout.grid_rows);
    setActiveLayoutId(layout.id);
    setLayoutName(layout.name);
    const newCells = {};
    if (Array.isArray(layout.cells)) {
      layout.cells.forEach(c => { newCells[String(c.cell_index)] = c.camera_id; });
    }
    setCells(newCells);
    setShowLayoutPanel(false);
  };

  const deleteLayout = async (id) => {
    const res = await apiClient.deleteVideoWallLayout?.(id);
    if (res?.success) {
      setLayouts(prev => prev.filter(l => l.id !== id));
      if (activeLayoutId === id) setActiveLayoutId(null);
    }
  };

  const clearCell = (cellIndex) => {
    setCells(prev => { const n = { ...prev }; delete n[String(cellIndex)]; return n; });
  };

  // Pop-out camera to new window (works in browser + Electron)
  const popOutCamera = (cameraId) => {
    const cam = cameras.find(c => c.id === cameraId);
    const title = cam ? cam.name : `Camera ${cameraId}`;
    
    // If in Electron, use IPC
    if (window.electronAPI?.popOutCamera) {
      window.electronAPI.popOutCamera(cameraId);
      return;
    }
    
    // Browser fallback: open in new window
    const w = window.open('', `cam_${cameraId}`, 'width=800,height=600,menubar=no,toolbar=no');
    if (!w) return;
    w.document.title = title;
    w.document.body.style.cssText = 'margin:0;padding:0;background:#000;overflow:hidden;';
    w.document.body.innerHTML = `
      <div id="root" style="width:100vw;height:100vh;"></div>
      <script type="module">
        // Dynamically load the camera feed
        const iframe = document.createElement('iframe');
        iframe.src = '${window.location.origin}/?embed=cam&id=${cameraId}';
        iframe.style.cssText = 'width:100%;height:100%;border:none;';
        document.getElementById('root').appendChild(iframe);
      </script>
    `;
  };

  // Drag and drop
  const handleDragStart = (e, cameraId) => {
    e.dataTransfer.setData('text/plain', String(cameraId));
    e.dataTransfer.effectAllowed = 'copy';
  };

  const handleDrop = (e, cellIndex) => {
    e.preventDefault();
    setDragOverCell(null);
    const camId = parseInt(e.dataTransfer.getData('text/plain'));
    if (!isNaN(camId)) {
      setCells(prev => ({ ...prev, [String(cellIndex)]: camId }));
    }
  };

  const handleDragOver = (e, cellIndex) => {
    e.preventDefault();
    e.dataTransfer.dropEffect = 'copy';
    setDragOverCell(cellIndex);
  };

  // Apply grid preset
  const applyPreset = (preset) => {
    setGridCols(preset.cols);
    setGridRows(preset.rows);
    // Keep existing cell assignments that fit
    const newCells = {};
    const max = preset.cols * preset.rows;
    Object.entries(cells).forEach(([idx, cam]) => {
      if (parseInt(idx) < max) newCells[idx] = cam;
    });
    setCells(newCells);
  };

  // Zoom cell
  const handleCellDoubleClick = (cellIndex) => {
    setZoomCell(prev => prev === cellIndex ? null : cellIndex);
  };

  const getCameraForCell = (idx) => {
    const camId = cells[String(idx)];
    if (!camId) return null;
    return cameras.find(c => c.id === camId);
  };

  return (
    <div className="view-panel active" id="view-videowall" ref={wallRef} style={{ display: 'flex', flexDirection: 'column', height: '100%', background: isFullscreen ? '#000' : undefined }}>
      {/* Toolbar */}
      <div className="grid-toolbar" style={{ justifyContent: 'space-between', flexShrink: 0 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
          <svg viewBox="0 0 14 14" fill="currentColor" style={{ width: 14, height: 14, opacity: 0.7 }}>
            <rect x="1" y="1" width="3" height="3" rx="0.5"/><rect x="5.5" y="1" width="3" height="3" rx="0.5"/>
            <rect x="10" y="1" width="3" height="3" rx="0.5"/><rect x="1" y="5.5" width="3" height="3" rx="0.5"/>
            <rect x="5.5" y="5.5" width="3" height="3" rx="0.5"/><rect x="10" y="5.5" width="3" height="3" rx="0.5"/>
            <rect x="1" y="10" width="3" height="3" rx="0.5"/><rect x="5.5" y="10" width="3" height="3" rx="0.5"/>
            <rect x="10" y="10" width="3" height="3" rx="0.5"/>
          </svg>
          <span style={{ fontFamily: "'Rajdhani', sans-serif", fontWeight: 700, fontSize: 12, letterSpacing: 2, color: 'var(--accent)' }}>VIDEO WALL</span>
          <div className="toolbar-sep2"></div>

          {/* Grid presets */}
          {GRID_PRESETS.map(p => (
            <div key={p.label}
                 className={`grid-btn ${gridCols === p.cols && gridRows === p.rows ? 'active' : ''}`}
                 onClick={() => applyPreset(p)}>
              {p.label}
            </div>
          ))}

          <div className="toolbar-sep2"></div>

          {/* Custom grid */}
          <span style={{ fontSize: 9, color: 'var(--text-dim)' }}>COLS</span>
          <input type="number" min={1} max={8} value={gridCols}
                 onChange={e => setGridCols(Math.max(1, Math.min(8, parseInt(e.target.value) || 1)))}
                 style={{ width: 32, background: 'rgba(255,255,255,0.05)', border: '1px solid rgba(255,255,255,0.1)', borderRadius: 3, color: '#fff', textAlign: 'center', fontSize: 11, padding: '2px 4px' }} />
          <span style={{ fontSize: 9, color: 'var(--text-dim)' }}>×</span>
          <span style={{ fontSize: 9, color: 'var(--text-dim)' }}>ROWS</span>
          <input type="number" min={1} max={8} value={gridRows}
                 onChange={e => setGridRows(Math.max(1, Math.min(8, parseInt(e.target.value) || 1)))}
                 style={{ width: 32, background: 'rgba(255,255,255,0.05)', border: '1px solid rgba(255,255,255,0.1)', borderRadius: 3, color: '#fff', textAlign: 'center', fontSize: 11, padding: '2px 4px' }} />
        </div>

        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
          <div className="grid-btn" onClick={() => setShowLayoutPanel(!showLayoutPanel)} title="Layouts">
            <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5" style={{ width: 12, height: 12 }}>
              <rect x="2" y="2" width="10" height="10" rx="1"/><path d="M2 5h10M5 5v7"/>
            </svg>
            LAYOUTS
          </div>
          <div className="grid-btn" onClick={saveLayout} title="Save">
            <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5" style={{ width: 12, height: 12 }}>
              <path d="M3 1h6l3 3v8a1 1 0 0 1-1 1H3a1 1 0 0 1-1-1V2a1 1 0 0 1 1-1z"/><path d="M5 1v4h4V1"/><path d="M4 9h6"/>
            </svg>
            SAVE
          </div>
          <div className="grid-btn" onClick={toggleFullscreen} title="Fullscreen" style={{ color: isFullscreen ? 'var(--accent)' : undefined }}>
            <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5" style={{ width: 12, height: 12 }}>
              <path d="M1 5V1h4M9 1h4v4M13 9v4H9M5 13H1V9"/>
            </svg>
            {isFullscreen ? 'EXIT' : 'FULLSCREEN'}
          </div>
        </div>
      </div>

      {/* Layout panel */}
      {showLayoutPanel && (
        <div style={{ position: 'absolute', top: 40, right: 10, width: 280, background: 'var(--panel)', border: '1px solid rgba(255,255,255,0.08)', borderRadius: 8, padding: 12, zIndex: 100, boxShadow: '0 8px 32px rgba(0,0,0,0.5)' }}>
          <div style={{ fontWeight: 700, fontSize: 11, letterSpacing: 1, color: 'var(--text-secondary)', marginBottom: 10, display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
            SAVED LAYOUTS
            <span style={{ cursor: 'pointer', color: 'var(--text-dim)' }} onClick={() => setShowLayoutPanel(false)}>✕</span>
          </div>
          <input
            type="text"
            placeholder="Layout name..."
            value={layoutName}
            onChange={e => setLayoutName(e.target.value)}
            style={{ width: '100%', background: 'rgba(255,255,255,0.05)', border: '1px solid rgba(255,255,255,0.1)', borderRadius: 4, color: '#fff', fontSize: 11, padding: '6px 8px', marginBottom: 8 }}
          />
          {layouts.length === 0 && (
            <div style={{ color: 'var(--text-dim)', fontSize: 10, textAlign: 'center', padding: 16 }}>
              No saved layouts yet
            </div>
          )}
          {layouts.map(l => (
            <div key={l.id} style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '6px 8px', borderRadius: 4, cursor: 'pointer', background: activeLayoutId === l.id ? 'rgba(0,200,245,0.1)' : 'transparent', border: activeLayoutId === l.id ? '1px solid rgba(0,200,245,0.2)' : '1px solid transparent', marginBottom: 4 }}
                 onClick={() => loadLayout(l)}>
              <div>
                <div style={{ fontSize: 11, fontWeight: 600, color: '#fff' }}>{l.name}</div>
                <div style={{ fontSize: 9, color: 'var(--text-dim)' }}>{l.grid_cols}×{l.grid_rows} · {Array.isArray(l.cells) ? l.cells.length : 0} cameras</div>
              </div>
              <span style={{ color: 'var(--danger)', cursor: 'pointer', fontSize: 10 }} onClick={e => { e.stopPropagation(); deleteLayout(l.id); }}>DEL</span>
            </div>
          ))}
        </div>
      )}

      {/* Camera list sidebar for drag & drop */}
      <div style={{ display: 'flex', flex: 1, overflow: 'hidden' }}>
        <div style={{ width: 160, borderRight: '1px solid rgba(255,255,255,0.06)', overflowY: 'auto', flexShrink: 0, padding: '8px 6px', background: 'rgba(0,0,0,0.2)' }}>
          <div style={{ fontSize: 9, fontWeight: 700, letterSpacing: 1, color: 'var(--text-dim)', marginBottom: 6 }}>DRAG CAMERAS</div>
          {cameras.map(cam => (
            <div key={cam.id}
                 draggable
                 onDragStart={e => handleDragStart(e, cam.id)}
                 style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '5px 6px', borderRadius: 4, cursor: 'grab', fontSize: 10, color: '#ccc', marginBottom: 2, background: 'rgba(255,255,255,0.03)', border: '1px solid rgba(255,255,255,0.05)', transition: 'all 0.15s' }}
                 onMouseOver={e => e.currentTarget.style.background = 'rgba(0,200,245,0.08)'}
                 onMouseOut={e => e.currentTarget.style.background = 'rgba(255,255,255,0.03)'}>
              <div style={{ width: 6, height: 6, borderRadius: '50%', backgroundColor: cam.status === 'online' ? '#0f0' : '#f00', flexShrink: 0 }}></div>
              <div style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{cam.name}</div>
            </div>
          ))}
        </div>

        {/* Grid */}
        <div style={{
          flex: 1,
          display: 'grid',
          gridTemplateColumns: zoomCell !== null ? '1fr' : `repeat(${gridCols}, 1fr)`,
          gridTemplateRows: zoomCell !== null ? '1fr' : `repeat(${gridRows}, 1fr)`,
          gap: 2,
          padding: 2,
          background: '#0a0a0a',
          overflow: 'hidden'
        }}>
          {zoomCell !== null ? (
            // Zoomed single cell
            <div style={{ position: 'relative', cursor: 'zoom-out' }}
                 onDoubleClick={() => setZoomCell(null)}>
              {cells[String(zoomCell)] ? (
                <>
                  <CameraFeed cameraId={cells[String(zoomCell)]} showControls={false} />
                  <div style={{ position: 'absolute', top: 6, left: 8, fontSize: 10, color: 'var(--accent)', fontWeight: 700, textShadow: '0 1px 4px #000', zIndex: 10 }}>
                    {getCameraForCell(zoomCell)?.name || `Camera ${cells[String(zoomCell)]}`}
                  </div>
                  <div style={{ position: 'absolute', top: 6, right: 8, display: 'flex', gap: 4, zIndex: 10 }}>
                    <div style={{ background: 'rgba(0,0,0,0.6)', borderRadius: 3, padding: '2px 6px', cursor: 'pointer', fontSize: 9, color: '#aaa' }} onClick={() => setZoomCell(null)}>EXIT ZOOM</div>
                  </div>
                </>
              ) : (
                <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100%', color: 'var(--text-dim)', fontSize: 11 }}>Empty Cell</div>
              )}
            </div>
          ) : (
            // Normal grid
            Array.from({ length: totalCells }).map((_, idx) => {
              const cam = getCameraForCell(idx);
              const isDragOver = dragOverCell === idx;
              return (
                <div key={idx}
                     onDrop={e => handleDrop(e, idx)}
                     onDragOver={e => handleDragOver(e, idx)}
                     onDragLeave={() => setDragOverCell(null)}
                     onDoubleClick={() => handleCellDoubleClick(idx)}
                     style={{
                       position: 'relative',
                       background: isDragOver ? 'rgba(0,200,245,0.15)' : '#111',
                       border: isDragOver ? '2px dashed var(--accent)' : '1px solid rgba(255,255,255,0.04)',
                       borderRadius: 3,
                       overflow: 'hidden',
                       transition: 'all 0.15s',
                       cursor: cam ? 'zoom-in' : 'default',
                       minHeight: 0
                     }}>
                  {cam ? (
                    <>
                      <CameraFeed cameraId={cam.id} showControls={false} />
                      {/* Overlay */}
                      <div style={{ position: 'absolute', inset: 0, pointerEvents: 'none', zIndex: 5 }}>
                        <div style={{ position: 'absolute', top: 3, left: 5, fontSize: gridCols >= 6 ? 7 : 9, color: 'rgba(255,255,255,0.8)', fontWeight: 700, textShadow: '0 1px 3px #000' }}>
                          {cam.name}
                        </div>
                        <div style={{ position: 'absolute', top: 3, right: 5, fontSize: 7, color: 'rgba(0,200,245,0.6)' }}>
                          <span style={{ display: 'inline-block', width: 4, height: 4, borderRadius: '50%', background: cam.status === 'online' ? '#0f0' : '#f00', marginRight: 3 }}></span>
                          {gridCols < 6 && (cam.status === 'online' ? 'LIVE' : 'OFF')}
                        </div>
                      </div>
                      {/* Controls */}
                      <div style={{ position: 'absolute', bottom: 3, right: 5, display: 'flex', gap: 3, zIndex: 10, pointerEvents: 'auto' }}>
                        <div style={{ background: 'rgba(0,0,0,0.6)', borderRadius: 2, padding: '1px 4px', cursor: 'pointer', fontSize: 7, color: '#aaa' }}
                             onClick={e => { e.stopPropagation(); popOutCamera(cam.id); }} title="Pop-out window">
                          ↗
                        </div>
                        <div style={{ background: 'rgba(0,0,0,0.6)', borderRadius: 2, padding: '1px 4px', cursor: 'pointer', fontSize: 7, color: '#f66' }}
                             onClick={e => { e.stopPropagation(); clearCell(idx); }} title="Remove">
                          ✕
                        </div>
                      </div>
                    </>
                  ) : (
                    <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100%', color: 'rgba(255,255,255,0.1)', fontSize: gridCols >= 6 ? 8 : 10, flexDirection: 'column', gap: 4 }}>
                      <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1" style={{ width: 14, height: 14, opacity: 0.3 }}>
                        <path d="M7 4v6M4 7h6"/>
                      </svg>
                      {gridCols < 6 && 'Drop camera'}
                    </div>
                  )}
                </div>
              );
            })
          )}
        </div>
      </div>
    </div>
  );
};

export default VideoWallView;
