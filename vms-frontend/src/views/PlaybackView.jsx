import React, { useState, useEffect, useRef, useMemo, useCallback } from 'react';
import useVmsStore from '../store/useVmsStore';
import apiClient from '../api/apiClient';

const API_BASE = ''; 
const SPEEDS = [0.25, 0.5, 1, 2, 4, 8, 16];
const ASSUMED_FPS = 25;

/**
 * Individual Camera Player for Playback
 */
const PlaybackPlayer = ({ camId, camName, startTime, endTime, segments, isPlaying, playbackSpeed, globalTime, onTimeUpdate, volume }) => {
  const videoRef = useRef(null);
  const [currentSeg, setCurrentSeg] = useState(null);

  // Sync with global time
  useEffect(() => {
    if (!videoRef.current || segments.length === 0) return;

    // Find segment for global time
    const seg = segments.find(s => globalTime >= s.start_time && globalTime <= s.end_time);
    
    if (seg) {
      const offset = globalTime - seg.start_time;
      if (currentSeg?.id !== seg.id) {
        setCurrentSeg(seg);
        videoRef.current.src = `${API_BASE}${seg.video_url}#t=${offset}`;
        videoRef.current.load();
        if (isPlaying) videoRef.current.play();
      } else {
        // If drift is too much (> 2.0s), resync
        // 0.5s is too tight and causes continuous stuttering as the setInterval clock drifts from the native media clock.
        const drift = Math.abs(videoRef.current.currentTime - offset);
        if (drift > 2.0 && isPlaying) {
          videoRef.current.currentTime = offset;
        } else if (!isPlaying && drift > 0.5) {
          // When paused, we can seek more tightly to scrub the timeline smoothly.
          videoRef.current.currentTime = offset;
        }
      }
    } else {
      setCurrentSeg(null);
      videoRef.current.pause();
      videoRef.current.removeAttribute('src');
    }
  }, [globalTime, segments, isPlaying]);

  // Sync Play/Pause
  useEffect(() => {
    if (!videoRef.current || !currentSeg) return;
    if (isPlaying && videoRef.current.paused) videoRef.current.play().catch(()=>{});
    if (!isPlaying && !videoRef.current.paused) videoRef.current.pause();
  }, [isPlaying]);

  // Sync Speed
  useEffect(() => {
    if (videoRef.current) videoRef.current.playbackRate = playbackSpeed;
  }, [playbackSpeed]);

  return (
    <div className="pb-screen" style={{position:'relative', overflow:'hidden', background:'#000', border: '1px solid var(--border)'}}>
      <video 
        ref={videoRef}
        style={{width: '100%', height: '100%', objectFit: 'contain'}}
        onTimeUpdate={() => {
           if (videoRef.current && currentSeg) {
              // Only one player should drive the global clock (handled in parent)
           }
        }}
        muted={volume === 0}
      />
      {!currentSeg && (
        <div style={{position:'absolute', inset:0, display:'flex', alignItems:'center', justifyContent:'center', color:'var(--text-dim)', fontSize:'10px', textAlign:'center', padding: 20}}>
          KHÔNG CÓ DỮ LIỆU
        </div>
      )}
      <div style={{position:'absolute', top:'5px', left:'5px', fontFamily:"'Share Tech Mono',monospace", fontSize:'10px', color:'rgba(255,255,255,0.7)', textShadow:'0 0 4px #000', background: 'rgba(0,0,0,0.4)', padding: '2px 5px'}}>
        {camName}
      </div>
    </div>
  );
};

const PlaybackView = () => {
  const cameras = useVmsStore((state) => state.cameras);
  
  const [selectedCamIds, setSelectedCamIds] = useState([]);
  const [selectedDate, setSelectedDate] = useState(new Date().toISOString().split('T')[0]);
  const [camerasData, setCamerasData] = useState({}); 
  const [isPlaying, setIsPlaying] = useState(false);
  const [playbackSpeed, setPlaybackSpeed] = useState(1);
  const [speedIdx, setSpeedIdx] = useState(2); 
  const [currentGlobalTime, setCurrentGlobalTime] = useState(0); 
  const [isSyncEnabled, setIsSyncEnabled] = useState(true);
  const [volume, setVolume] = useState(0);
  const [isFullscreen, setIsFullscreen] = useState(false);
  const [isLoading, setIsLoading] = useState(false);
  
  const timelineRef = useRef(null);
  const playerContainerRef = useRef(null);

  useEffect(() => {
    if (cameras.length > 0 && selectedCamIds.length === 0) {
      setSelectedCamIds([cameras[0].id]);
    }
  }, [cameras]);

  const dayBoundary = useMemo(() => {
    const d = new Date(selectedDate);
    const startOfDay = Math.floor(new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime() / 1000);
    const endOfDay = startOfDay + 86400;
    return { startOfDay, endOfDay };
  }, [selectedDate]);

  const handleSearch = useCallback(async () => {
    if (selectedCamIds.length === 0) return;
    setIsLoading(true);
    const newData = { ...camerasData };
    try {
      await Promise.all(selectedCamIds.map(async (camId) => {
        const segRes = await apiClient.getRecordingSegments({ camera_id: camId, date: selectedDate });
        const evtRes = await apiClient.getEvents({ camera_id: camId, start_time: dayBoundary.startOfDay, end_time: dayBoundary.endOfDay, limit: 1000 });
        newData[camId] = {
          segments: segRes.success ? (segRes.data?.segments || []) : [],
          events: evtRes.success ? (evtRes.data?.events || []) : [],
        };
      }));
      setCamerasData(newData);
      if (currentGlobalTime === 0 || currentGlobalTime < dayBoundary.startOfDay || currentGlobalTime > dayBoundary.endOfDay) {
         setCurrentGlobalTime(dayBoundary.startOfDay);
      }
    } catch (e) {
      console.error("Failed to fetch playback data", e);
    } finally {
      setIsLoading(false);
    }
  }, [selectedCamIds, selectedDate, dayBoundary]);

  useEffect(() => {
    handleSearch();
  }, [selectedDate, selectedCamIds.length]);

  // Master Clock: Increments when playing
  useEffect(() => {
    let interval;
    if (isPlaying) {
      interval = setInterval(() => {
        setCurrentGlobalTime(prev => prev + (0.1 * playbackSpeed)); // Update every 100ms
      }, 100);
    }
    return () => clearInterval(interval);
  }, [isPlaying, playbackSpeed]);

  const handleTimelineClick = (e) => {
    if (!timelineRef.current) return;
    const rect = timelineRef.current.getBoundingClientRect();
    const percent = (e.clientX - rect.left) / rect.width;
    const targetTimestamp = dayBoundary.startOfDay + Math.floor(percent * 86400);
    setCurrentGlobalTime(targetTimestamp);
  };

  const skipTime = (amount) => setCurrentGlobalTime(prev => prev + amount);

  const togglePlay = () => setIsPlaying(!isPlaying);

  const changeSpeed = (direction) => {
    const newIdx = Math.max(0, Math.min(SPEEDS.length - 1, speedIdx + direction));
    setSpeedIdx(newIdx);
    setPlaybackSpeed(SPEEDS[newIdx]);
  };

  const formatTime = (ts) => {
    if (!ts) return "00:00:00";
    const d = new Date(ts * 1000);
    return d.toTimeString().split(' ')[0];
  };

  const cursorLeftPercent = useMemo(() => {
    const r = (currentGlobalTime - dayBoundary.startOfDay) / 86400;
    return Math.max(0, Math.min(100, r * 100));
  }, [currentGlobalTime, dayBoundary]);

  // Use the first cameras segments for the timeline background
  const primarySegments = useMemo(() => {
    if (selectedCamIds.length > 0 && camerasData[selectedCamIds[0]]) {
      return camerasData[selectedCamIds[0]].segments;
    }
    return [];
  }, [selectedCamIds, camerasData]);

  return (
    <div className="view-panel active" id="view-playback">
      <div className="playback-view">
        <div className="pb-toolbar">
          <span style={{fontSize:'10px', color:'var(--text-secondary)', fontFamily:"'Rajdhani',sans-serif", fontWeight:600, letterSpacing:'1px', marginRight: 10}}>LỊCH:</span>
          <input className="pb-date-input" type="date" value={selectedDate} onChange={e => setSelectedDate(e.target.value)} />
          <div className="toolbar-sep"></div>
          <div style={{display:'flex', gap:10, alignItems:'center'}}>
             <span style={{fontSize:10, color:'var(--text-dim)'}}>ĐỒNG BỘ:</span>
             <div className={`toggle ${isSyncEnabled ? 'on' : ''}`} onClick={() => setIsSyncEnabled(!isSyncEnabled)}>
                <div className="toggle-dot" />
             </div>
          </div>
          <div style={{marginLeft:'auto', display:'flex', gap:'6px', alignItems:'center'}}>
            <span style={{fontSize:'9px', color:'var(--text-dim)', fontFamily:"'Rajdhani',sans-serif", letterSpacing:'0.5px'}}>
              ENTERPRISE SYNC PLAYBACK ACTIVE
            </span>
          </div>
        </div>
        
        <div className="pb-main">
          <div className="pb-cam-list">
            <div style={{fontSize:'9px', color:'var(--text-dim)', letterSpacing:'1.5px', fontFamily:"'Rajdhani',sans-serif", fontWeight:700, textTransform:'uppercase', marginBottom:'8px'}}>Chọn Camera</div>
            <div style={{display:'flex', flexDirection:'column', gap:4}}>
              {cameras.map(cam => (
                <div 
                  key={cam.id}
                  className={`pb-cam-item ${selectedCamIds.includes(cam.id) ? 'selected' : ''}`}
                  onClick={() => {
                    if (selectedCamIds.includes(cam.id)) {
                      setSelectedCamIds(selectedCamIds.filter(id => id !== cam.id));
                    } else {
                      setSelectedCamIds([...selectedCamIds, cam.id]);
                    }
                  }}
                >
                  <div style={{
                    width: '12px', height: '12px', borderRadius: '50%',
                    border: `2px solid ${selectedCamIds.includes(cam.id) ? 'var(--accent)' : 'var(--text-dim)'}`,
                    background: selectedCamIds.includes(cam.id) ? 'var(--accent)' : 'transparent',
                    marginRight: '8px'
                  }}></div>
                  <span>{cam.name}</span>
                </div>
              ))}
            </div>
          </div>
          
          <div className="pb-player" ref={playerContainerRef}>
            <div style={{
              display: 'grid', 
              gridTemplateColumns: selectedCamIds.length > 4 ? 'repeat(3, 1fr)' : (selectedCamIds.length > 1 ? 'repeat(2, 1fr)' : '1fr'),
              gap: '4px',
              flex: 1,
              background: '#000',
              overflow: 'hidden'
            }}>
              {selectedCamIds.map(camId => (
                <PlaybackPlayer 
                  key={camId}
                  camId={camId}
                  camName={cameras.find(c => c.id === camId)?.name || 'Unknown'}
                  globalTime={currentGlobalTime}
                  isPlaying={isPlaying}
                  playbackSpeed={playbackSpeed}
                  segments={camerasData[camId]?.segments || []}
                  volume={volume}
                />
              ))}
            </div>
            
            <div className="pb-controls">
              <div className="tl-controls">
                 <div className="tl-btn" onClick={() => skipTime(-10)}><svg viewBox="0 0 12 12" fill="currentColor"><path d="M8 2L2 6l6 4z"/></svg></div>
                 <div className={`tl-btn ${isPlaying ? 'active' : ''}`} onClick={togglePlay} style={{width: 32, height: 32}}>
                    {isPlaying ? <svg viewBox="0 0 12 12" fill="currentColor"><path d="M3 2h2v8H3zm4 0h2v8H7z"/></svg> : <svg viewBox="0 0 12 12" fill="currentColor"><path d="M3 2L9 6 3 10z"/></svg>}
                 </div>
                 <div className="tl-btn" onClick={() => skipTime(10)}><svg viewBox="0 0 12 12" fill="currentColor"><path d="M4 2L10 6 4 10z"/></svg></div>
              </div>
              
              <div className="pb-tl-wrap">
                <div className="pb-tl" ref={timelineRef} onClick={handleTimelineClick}>
                  {primarySegments.map((seg, i) => {
                    const l = (seg.start_time - dayBoundary.startOfDay) / 86400 * 100;
                    const w = (seg.end_time - seg.start_time) / 86400 * 100;
                    return <div key={i} className="pb-tl-event" style={{ left: `${Math.max(0, l)}%`, width: `${Math.min(100-l, w)}%`, background: 'rgba(0,200,245,0.4)', height: '100%', bottom: 0 }}></div>;
                  })}
                  <div className="pb-tl-thumb" style={{left: `${cursorLeftPercent}%`, zIndex: 10}}></div>
                </div>
                <div className="pb-tl-labels">
                  <span className="pb-tl-label">00:00</span>
                  <span className="pb-tl-label">12:00</span>
                  <span className="pb-tl-label">24:00</span>
                </div>
              </div>

              <div style={{display:'flex', alignItems:'center', gap:'2px'}}>
                <div className="tl-btn" onClick={() => changeSpeed(-1)} style={{width:18, height:18}}><svg viewBox="0 0 12 12" fill="currentColor" style={{width:8}}><path d="M8 2L2 6l6 4z"/></svg></div>
                <div className="pb-speed" style={{minWidth:35}}>{playbackSpeed}×</div>
                <div className="tl-btn" onClick={() => changeSpeed(1)} style={{width:18, height:18}}><svg viewBox="0 0 12 12" fill="currentColor" style={{width:8}}><path d="M4 2L10 6 4 10z"/></svg></div>
              </div>

              <span style={{fontFamily:"'Share Tech Mono',monospace", fontSize:'12px', color:'var(--accent)', minWidth:'70px', textAlign:'right'}}>
                {formatTime(currentGlobalTime)}
              </span>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
};

export default PlaybackView;
