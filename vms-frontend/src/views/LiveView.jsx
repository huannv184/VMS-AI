import React, { useState, useEffect, useMemo, useCallback } from 'react';
import useVmsStore from '../store/useVmsStore';
import CameraFeed from '../components/CameraFeed';
import PtzPatrolModal from '../components/PtzPatrolModal';

// PERF-OPT: Memoized cell to prevent re-renders when sibling cameras update status
const CameraCell = React.memo(({ cam, timestamp, onPatrol }) => (
  <div className={`cam-cell ${cam.status === 'error' ? 'alert-cam' : ''}`}>
    <CameraFeed cameraId={cam.id} showControls={false} />
    <div className="cam-overlay">
      <div className="corner-tl"></div><div className="corner-tr"></div>
      <div className="corner-bl"></div><div className="corner-br"></div>
      <div className="cam-header">
        <span className="cam-title">{cam.name}</span>
        <span className="cam-rec"><span className="rec-dot"></span>REC</span>
        <div style={{marginLeft:'auto', display:'flex', gap:'5px'}}>
           <div style={{cursor:'pointer', opacity:0.8}}
                onClick={(e) => { e.stopPropagation(); onPatrol(cam.id); }}
                title="Tuần tra PTZ">
              <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5" style={{width:'12px',height:'12px'}}>
                <path d="M1 7h3M10 7h3M7 1v3M7 10v3"/><circle cx="7" cy="7" r="2.5"/>
              </svg>
           </div>
        </div>
      </div>
      <div className="cam-footer">
        <span className="cam-ts">{timestamp}</span>
        <span className="cam-fps">{cam.fps || 0}fps · {cam.resolution || '640x360'}</span>
      </div>
      <div className="ai-tag">AI·ON</div>
      {cam.status === 'error' && (
        <div style={{position:'absolute', inset:0, background:'rgba(255,48,96,0.1)', display:'flex', flexDirection:'column', alignItems:'center', justifyContent:'center', zIndex:10, backdropFilter:'blur(2px)'}}>
           <div style={{fontSize:'28px', marginBottom:'8px'}}>⚠️</div>
           <div style={{fontFamily:"'Rajdhani',sans-serif", fontWeight:700, fontSize:'12px', color:'var(--danger)', letterSpacing:'1px', textAlign:'center', textShadow:'0 0 10px rgba(0,0,0,0.8)'}}>LỖI MẤT KẾT NỐI</div>
        </div>
      )}
    </div>
  </div>
));

const LiveView = () => {
  const cameras = useVmsStore((state) => state.cameras);
  const [layout, setLayout] = useState(4);
  const [clockTick, setClockTick] = useState(0);
  
  // Touring State
  const [isTouring, setIsTouring] = useState(false);
  const [tourIndex, setTourIndex] = useState(0);
  const [tourInterval, setTourInterval] = useState(10); // seconds
  const [showPatrolCam, setShowPatrolCam] = useState(null);
  
  useEffect(() => {
    const timer = setInterval(() => setClockTick(t => t + 1), 1000);
    return () => clearInterval(timer);
  }, []);

  // Touring Engine
  useEffect(() => {
    let timer;
    if (isTouring && cameras.length > layout) {
      timer = setInterval(() => {
        setTourIndex(prev => (prev + layout) % cameras.length);
      }, tourInterval * 1000);
    } else {
      setTourIndex(0);
    }
    return () => clearInterval(timer);
  }, [isTouring, layout, cameras.length, tourInterval]);

  const timestamp = useMemo(() => {
    return new Date().toISOString().replace('T', ' ').substr(0, 19);
  }, [clockTick]);

  const visibleCameras = useMemo(() => {
    if (!isTouring) return cameras.slice(0, layout);
    // Cycle logic
    const start = tourIndex;
    let end = tourIndex + layout;
    let sliced = cameras.slice(start, end);
    // Wrap around if needed
    if (sliced.length < layout && cameras.length >= layout) {
       sliced = [...sliced, ...cameras.slice(0, layout - sliced.length)];
    }
    return sliced;
  }, [cameras, layout, isTouring, tourIndex]);

  // PERF-OPT: stable callback ref for patrol button
  const handlePatrol = useCallback((camId) => setShowPatrolCam(camId), []);

  return (
    <div className="view-panel active" id="view-live">
      <div className="grid-toolbar">
        <div className={`grid-btn ${layout === 6 ? 'active' : ''}`} onClick={() => setLayout(6)} title="2×3">
          <svg viewBox="0 0 14 14" fill="currentColor"><rect x="1" y="1" width="3.5" height="3.5"/><rect x="5.5" y="1" width="3.5" height="3.5"/><rect x="9.5" y="1" width="3.5" height="3.5"/><rect x="1" y="8" width="3.5" height="3.5"/><rect x="5.5" y="8" width="3.5" height="3.5"/><rect x="9.5" y="8" width="3.5" height="3.5"/></svg>
          2×3
        </div>
        <div className={`grid-btn ${layout === 4 ? 'active' : ''}`} onClick={() => setLayout(4)} title="2×2">
          <svg viewBox="0 0 14 14" fill="currentColor"><rect x="1" y="1" width="5" height="5"/><rect x="8" y="1" width="5" height="5"/><rect x="1" y="8" width="5" height="5"/><rect x="8" y="8" width="5" height="5"/></svg>
          2×2
        </div>
        <div className={`grid-btn ${layout === 1 ? 'active' : ''}`} onClick={() => setLayout(1)} title="1×1">
          <svg viewBox="0 0 14 14" fill="currentColor"><rect x="1" y="1" width="12" height="12" rx="1"/></svg>
          1×1
        </div>
        <div className={`grid-btn ${layout === 9 ? 'active' : ''}`} onClick={() => setLayout(9)} title="3×3">
          <svg viewBox="0 0 14 14" fill="currentColor"><rect x="1" y="1" width="3" height="3"/><rect x="5.5" y="1" width="3" height="3"/><rect x="10" y="1" width="3" height="3"/><rect x="1" y="5.5" width="3" height="3"/><rect x="5.5" y="5.5" width="3" height="3"/><rect x="10" y="5.5" width="3" height="3"/><rect x="1" y="10" width="3" height="3"/><rect x="5.5" y="10" width="3" height="3"/><rect x="10" y="10" width="3" height="3"/></svg>
          3×3
        </div>
        
        <div className="toolbar-sep2"></div>
        
        {/* TOURING BUTTON */}
        <div className={`grid-btn ${isTouring ? 'active' : ''}`} 
             style={{ color: isTouring ? 'var(--accent)' : 'inherit' }}
             onClick={() => setIsTouring(!isTouring)}
             title="Chế độ tuần tra (Auto Cycle)">
          <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5">
             <path d="M1 7h3M10 7h3M7 1v3M7 10v3"/>
             <circle cx="7" cy="7" r="2.5"/>
          </svg>
          {isTouring ? `TUẦN TRA (${tourInterval}s)` : 'TUẦN TRA'}
        </div>

        <div className="toolbar-sep2"></div>
        <div className="grid-btn" title="SNAPSHOT">
          <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><rect x="1" y="3" width="12" height="9" rx="1"/><circle cx="7" cy="7.5" r="2.5"/></svg>
          SNAPSHOT
        </div>
        <div className="grid-btn">
          <svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.5"><path d="M7 2v7M4.5 7L7 10l2.5-3"/><path d="M2 11h10"/></svg>
          EXPORT
        </div>
        <div className="toolbar-sep"></div>
        <div className="ai-badge">
          <div className="ai-dot-spin"></div>
          AI ACTIVE
        </div>
      </div>

      <div className={`cam-grid layout-${layout === 6 ? 'default' : layout}`} id="camGrid">
        {visibleCameras.map((cam) => (
          <CameraCell
            key={cam.id}
            cam={cam}
            timestamp={timestamp}
            onPatrol={handlePatrol}
          />
        ))}
        {cameras.length === 0 && (
          <div style={{gridColumn: '1 / -1', height: '400px', display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'var(--text-dim)', fontSize: '14px', letterSpacing: '2px'}}>
             HỆ THỐNG CHƯA CÓ CAMERA - VUI LÒNG THÊM MỚI TRONG PHẦN CÀI ĐẶT
          </div>
        )}
      </div>

      <div className="timeline-bar">
        <div className="tl-controls">
          <div className="tl-btn" title="Play/Pause">
            <svg viewBox="0 0 12 12" fill="currentColor"><path d="M3 2L9 6 3 10z"/></svg>
          </div>
        </div>
        <div className="timeline-track">
          <div className="tl-segment" style={{ left: '0%', width: '100%', background: 'rgba(0,200,245,0.1)' }}></div>
          <div className="tl-cursor" style={{left: '52%'}}></div>
        </div>
        <div className="tl-time">{new Date().toLocaleTimeString()}</div>
      </div>

      {showPatrolCam && (
        <PtzPatrolModal 
          cameraId={showPatrolCam} 
          onClose={() => setShowPatrolCam(null)} 
        />
      )}
    </div>
  );
};

export default LiveView;
