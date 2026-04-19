import React, { useState, useRef, useEffect, useMemo } from 'react';
import { Upload, Map as MapIcon, Camera, MousePointer2, Move, Trash2, Plus, PlusCircle, Maximize2, Layers, Save, RotateCw, X } from 'lucide-react';
import useVmsStore from '../store/useVmsStore';
import apiClient from '../api/apiClient';
import CameraFeed from '../components/CameraFeed';

const FloorMapView = () => {
  const cameras = useVmsStore((state) => state.cameras);
  const alarms = useVmsStore((state) => state.alarms);
  
  const [mapImage, setMapImage] = useState(null);
  const [markers, setMarkers] = useState([]);
  const [isEditMode, setIsEditMode] = useState(false);
  const [isSaving, setIsSaving] = useState(false);
  const [isDirty, setIsDirty] = useState(false);
  
  // Interactive States
  const [activeCameraId, setActiveCameraId] = useState(null);
  const [draggingMarkerId, setDraggingMarkerId] = useState(null);
  
  const mapRef = useRef(null);

  // Load from backend on mount
  useEffect(() => {
    apiClient.getSettings().then(res => {
      if (res.success && res.data?.cfgFloorMap) {
          try {
              const config = typeof res.data.cfgFloorMap === 'string' ? JSON.parse(res.data.cfgFloorMap) : res.data.cfgFloorMap;
              if (config.bgImage) setMapImage(config.bgImage);
              if (config.markers && Array.isArray(config.markers)) setMarkers(config.markers);
          } catch(e) {}
      }
    });
  }, []);

  const handleFileUpload = (e) => {
    const file = e.target.files[0];
    if (file && file.size < 2 * 1024 * 1024) {
      const reader = new FileReader();
      reader.onload = (event) => { setMapImage(event.target.result); setIsDirty(true); };
      reader.readAsDataURL(file);
    }
  };

  const handleDragStart = (e, cam) => {
      e.dataTransfer.setData('camId', cam.id);
  };

  const handleDrop = (e) => {
      e.preventDefault();
      if (!isEditMode || !mapRef.current) return;
      
      const camIdStr = e.dataTransfer.getData('camId');
      if (!camIdStr) return;
      const camId = parseInt(camIdStr);
      const camInfo = cameras.find(c => c.id === camId);
      if (!camInfo) return;

      const rect = mapRef.current.getBoundingClientRect();
      const x = ((e.clientX - rect.left) / rect.width) * 100;
      const y = ((e.clientY - rect.top) / rect.height) * 100;

      const newMarker = {
        id: `cam-${camInfo.id}`,
        db_id: camInfo.id,
        x, y,
        name: camInfo.name?.toUpperCase() || `CAM-${camInfo.id}`,
        type: 'camera',
        angle: 0
      };
      
      setMarkers(prev => {
          const removed = prev.filter(m => m.id !== newMarker.id);
          return [...removed, newMarker];
      });
      setIsDirty(true);
  };

  const handleMouseMove = (e) => {
    if (!isEditMode || !draggingMarkerId || !mapRef.current) return;
    const rect = mapRef.current.getBoundingClientRect();
    const x = ((e.clientX - rect.left) / rect.width) * 100;
    const y = ((e.clientY - rect.top) / rect.height) * 100;

    setMarkers(prev => prev.map(m => 
      m.id === draggingMarkerId ? { ...m, x, y } : m
    ));
    setIsDirty(true);
  };

  const saveMapConfig = async () => {
    setIsSaving(true);
    try {
        const res = await apiClient.updateSettings({ cfgFloorMap: { bgImage: mapImage, markers } });
        if (res.success) {
          setIsDirty(false);
          alert('Lưu bản đồ thành công!');
        } else {
          alert(res.error || 'Lỗi khi lưu bản đồ.');
        }
    } catch(e) {
        alert('Lỗi khi lưu bản đồ.');
    } finally {
        setIsSaving(false);
    }
  };

  return (
    <div className="analytics-view active" style={{ background: '#030508', position: 'relative', overflow:'hidden' }} onMouseUp={() => setDraggingMarkerId(null)}>
      
      {/* CSS For Animations */}
      <style>{`
        @keyframes alert-pulse {
          0% { box-shadow: 0 0 0 0 rgba(255, 48, 96, 0.7); }
          70% { box-shadow: 0 0 0 15px rgba(255, 48, 96, 0); }
          100% { box-shadow: 0 0 0 0 rgba(255, 48, 96, 0); }
        }
        .marker-alert { animation: alert-pulse 1.5s infinite; border-radius: 50%; }
        .fov-cone { opacity: 0.2; transition: transform 0.3s ease; pointer-events: none; }
      `}</style>

      {/* MAP TOOLBAR */}
      <div className="grid-toolbar" style={{ zIndex: 100 }}>
        <div className={`grid-btn ${!isEditMode ? 'active' : ''}`} onClick={() => setIsEditMode(false)}>
           <MousePointer2 size={13} style={{marginRight: 5}}/> XEM BẢN ĐỒ
        </div>
        <div className={`grid-btn ${isEditMode ? 'active' : ''}`} onClick={() => setIsEditMode(true)}>
           <PlusCircle size={13} style={{marginRight: 5}}/> BIÊN TẬP
        </div>
        <div className="toolbar-sep2"></div>
        {isEditMode && (
          <>
            <label className="grid-btn" style={{ cursor: 'pointer' }}>
               <Upload size={13} style={{marginRight: 5}}/> ẢNH NỀN
               <input type="file" hidden onChange={handleFileUpload} accept="image/*" />
            </label>
            <div className={`grid-btn ${isDirty ? 'active' : ''}`} onClick={saveMapConfig}>
               {isSaving ? "ĐANG LƯU..." : <Save size={13} style={{marginRight: 5}}/>} LƯU LẠI
            </div>
          </>
        )}
      </div>

      <div style={{ display: 'flex', flex: 1, height: 'calc(100% - 40px)', overflow: 'hidden' }}>
        
        {/* SIDEBAR */}
        {isEditMode && (
          <div className="pb-cam-list" style={{ width: 180, background: 'rgba(10,20,30,0.8)', borderRight:'1px solid var(--border)' }}>
            <div className="sidebar-section-title">CAMERA CHƯA ĐẶT</div>
            {cameras.filter(c => !markers.find(m => m.db_id === c.id)).map((cam) => (
               <div key={cam.id} className="tree-item" draggable onDragStart={(e) => handleDragStart(e, cam)} style={{ fontSize: 10, cursor: 'grab', padding: '10px', border: '1px dashed rgba(0,245,255,0.2)', marginBottom: '8px', borderRadius: '4px' }}>
                  <Camera size={12} style={{ color: cam.status === 'online' ? 'var(--accent)' : 'var(--danger)', marginRight: '8px' }} /> {cam.name}
               </div>
            ))}
          </div>
        )}

        {/* MAP AREA */}
        <div 
          ref={mapRef}
          onDrop={handleDrop}
          onDragOver={(e) => e.preventDefault()}
          onMouseMove={handleMouseMove}
          style={{ 
            flex: 1, position: 'relative', overflow: 'hidden', 
            background: 'radial-gradient(circle, #0a1525 0%, #030508 100%)',
            display:'flex', alignItems:'center', justifyContent:'center'
          }}
        >
          <div style={{ position:'absolute', inset:0, backgroundImage: 'radial-gradient(rgba(0,200,245,0.05) 1px, transparent 1px)', backgroundSize: '30px 30px' }}></div>

          {mapImage ? (
            <img src={mapImage} alt="Map" style={{ maxWidth: '95%', maxHeight: '95%', objectFit: 'contain', opacity: 0.6 }} />
          ) : (
            <div style={{ color: 'var(--text-dim)', textAlign:'center' }}>
               <MapIcon size={64} style={{opacity:0.2, marginBottom:10}} />
               <div style={{fontSize:10, letterSpacing:2}}>CHƯA CÓ BẢN ĐỒ</div>
            </div>
          )}

          {/* MARKERS */}
          {markers.map(m => {
            const liveCam = cameras.find(c => c.id === m.db_id);
            const isOnline = liveCam ? (liveCam.status === 'online') : false;
            const hasAlarm = alarms.some(a => a.camera_id === m.db_id && (Date.now() - new Date(a.timestamp).getTime() < 30000));

            return (
              <div 
                key={m.id}
                style={{ position: 'absolute', left: `${m.x}%`, top: `${m.y}%`, transform: 'translate(-50%, -50%)', zIndex: 10 }}
              >
                {/* FOV CONE */}
                <div 
                  className="fov-cone"
                  style={{
                    position: 'absolute', width: 120, height: 120, top: -60, left: -60,
                    background: `conic-gradient(from ${m.angle - 30}deg, transparent 0, rgba(0, 200, 245, 0.15) 30deg, transparent 60deg)`,
                    borderRadius: '50%', transform: `rotate(${m.angle}deg)`
                  }}
                ></div>

                <div 
                  className={`fm-cam ${hasAlarm ? 'marker-alert' : ''}`} 
                  style={{ cursor: isEditMode ? 'move' : 'pointer', pointerEvents: 'auto' }}
                  onMouseDown={(e) => { if (isEditMode) { e.stopPropagation(); setDraggingMarkerId(m.id); } }}
                  onClick={() => !isEditMode && setActiveCameraId(m.db_id)}
                >
                  <div style={{
                    width: 32, height: 32, borderRadius: '50%', 
                    background: isOnline ? 'rgba(0,200,245,0.2)' : 'rgba(255,48,96,0.1)',
                    border: `1px solid ${isOnline ? 'var(--accent)' : 'var(--danger)'}`,
                    display:'flex', alignItems:'center', justifyContent:'center',
                    position:'relative'
                  }}>
                    <Camera size={14} color={isOnline ? 'var(--accent)' : 'var(--danger)'} />
                    
                    {isEditMode && (
                      <div 
                        style={{ position:'absolute', bottom:-15, right:-15, background:'var(--bg-panel)', padding:2, borderRadius:2, border:'1px solid var(--border)' }}
                        onClick={(e) => { 
                          e.stopPropagation(); 
                          setMarkers(markers.map(x => x.id === m.id ? { ...x, angle: (x.angle + 45) % 360 } : x));
                          setIsDirty(true);
                        }}
                      >
                         <RotateCw size={10} />
                      </div>
                    )}
                    {isEditMode && (
                       <div 
                         style={{ position:'absolute', top:-15, right:-15, color:'var(--danger)' }}
                         onClick={(e) => { e.stopPropagation(); setMarkers(markers.filter(x => x.id !== m.id)); setIsDirty(true); }}
                       >
                          <X size={12} />
                       </div>
                    )}
                  </div>
                  <div style={{ position:'absolute', top:35, left:'50%', transform:'translateX(-50%)', fontSize:8, whiteSpace:'nowrap', color:'#fff', background:'rgba(0,0,0,0.5)', padding:'1px 4px' }}>
                    {m.name}
                  </div>
                </div>
              </div>
            );
          })}

          {/* MINI PREVIEW POPUP */}
          {activeCameraId && (
            <div className="glass" style={{ position: 'absolute', bottom: 20, left: 20, width: 320, height: 200, zIndex: 100, borderRadius: 8, overflow: 'hidden', border: '1px solid var(--accent)' }}>
               <div style={{ position:'absolute', top:0, left:0, right:0, height:24, background:'rgba(0,0,0,0.7)', zIndex:10, display:'flex', alignItems:'center', justifyContent:'space-between', padding:'0 10px' }}>
                  <span style={{ fontSize:10, fontWeight:600 }}>LIVE PREVIEW: {cameras.find(c => c.id === activeCameraId)?.name}</span>
                  <X size={12} style={{ cursor:'pointer' }} onClick={() => setActiveCameraId(null)} />
               </div>
               <CameraFeed cameraId={activeCameraId} showControls={false} />
            </div>
          )}

        </div>
      </div>
    </div>
  );
};

export default FloorMapView;
