import React, { useState, useEffect, useRef, useCallback } from 'react';
import useVmsStore from '../store/useVmsStore';
import { resolveWsUrl } from '../utils/streamingConfig';
import apiClient from '../api/apiClient';

// ─── Constants ──────────────────────────────────────────
const PROTOCOL_MAGIC = 0x564D5331;
const PROTOCOL_VERSION_WITH_LENGTH = 3;
const WS_FALLBACK_TIMEOUT = 15000;
const HTTP_FRAME_INTERVAL = 150;
const FRAME_TYPE_JPEG = 1;
const FRAME_TYPE_H264 = 2;
const FRAME_TYPE_FMP4 = 3;
const MAX_SEGMENT_QUEUE = 4;
const WS_RECONNECT_BASE_MS = 2000;
const WS_RECONNECT_MAX_MS = 15000;

// ─── Binary Protocol Parser ────────────────────────────
function parseStreamFrame(data) {
  try {
    const view = new DataView(data);
    if (view.byteLength < 16) return null;
    if (view.getUint32(0) !== PROTOCOL_MAGIC) return null;
    const version = view.getUint8(4);
    const frameType = view.getUint8(5);
    const flags = view.getUint8(6);
    const headerLength = version >= PROTOCOL_VERSION_WITH_LENGTH ? view.getUint8(7) : 16;
    if (headerLength < 16 || view.byteLength < headerLength) return null;
    const seq = view.getUint32(8);
    const metaLen = view.getUint32(12);
    const payloadLen = version >= PROTOCOL_VERSION_WITH_LENGTH
      ? view.getUint32(16)
      : view.byteLength - headerLength - metaLen;
    if (payloadLen < 0) return null;
    if (view.byteLength !== headerLength + metaLen + payloadLen) return null;
    let metadata = null;
    if (metaLen > 0) {
      try { metadata = JSON.parse(new TextDecoder().decode(new Uint8Array(data, headerLength, metaLen))); } catch {}
    }
    const payload = new Uint8Array(data, headerLength + metaLen, payloadLen);
    return { metadata, payload, frameType, flags, seq };
  } catch { return null; }
}

const TRACK_COLORS = [
  '#00c8f5', '#00f53d', '#f5d400', '#f500d4', '#ff7300', 
  '#a200ff', '#00f5a2', '#7fff00', '#00e5ff', '#ff00aa'
];

function drawDetections(ctx, detections, canvasW, canvasH, sourceW = 640, sourceH = 360) {
  if (!detections || detections.length === 0) return;
  ctx.lineWidth = 2;
  ctx.font = 'bold 11px "Share Tech Mono", monospace';
  for (const obj of detections) {
    const bbox = obj.bbox;
    if (!bbox) continue;
    let x, y, w, h;
    if (Array.isArray(bbox)) {
      x = (bbox[0] / sourceW) * canvasW;
      y = (bbox[1] / sourceH) * canvasH;
      w = ((bbox[2] - bbox[0]) / sourceW) * canvasW;
      h = ((bbox[3] - bbox[1]) / sourceH) * canvasH;
    } else {
      x = (bbox.x || 0) * canvasW;
      y = (bbox.y || 0) * canvasH;
      w = (bbox.width || 0) * canvasW;
      h = (bbox.height || 0) * canvasH;
    }
    
    let color = '#00c8f5';
    if (obj.alert) {
      color = '#ff3060';
    } else if (obj.track_id != null && obj.track_id >= 0) {
      color = TRACK_COLORS[obj.track_id % TRACK_COLORS.length];
    } else if (obj.id != null && typeof obj.id === 'number' && obj.id >= 0) {
      color = TRACK_COLORS[obj.id % TRACK_COLORS.length];
    }

    const label = obj.label || obj.class_name || 'Object';
    ctx.strokeStyle = color;
    ctx.strokeRect(x, y, w, h);
    const text = obj.confidence != null ? `${label} ${(obj.confidence * 100).toFixed(0)}%` : label;
    const tw = ctx.measureText(text).width;
    ctx.fillStyle = color;
    ctx.fillRect(x, (y - 16 > 0 ? y - 16 : y), tw + 8, 16);
    ctx.fillStyle = '#fff';
    ctx.fillText(text, x + 4, (y - 16 > 0 ? y - 16 : y) + 12);
  }
}

const CameraFeed = React.memo(({ cameraId, showControls = true }) => {
  const canvasRef = useRef(null);
  const videoRef = useRef(null);
  const wsRef = useRef(null);
  const reconnectTimerRef = useRef(null);
  const fallbackTimerRef = useRef(null);
  const httpTimerRef = useRef(null);
  const mountedRef = useRef(true);
  const wsFrameReceived = useRef(false);
  const rafRef = useRef(null);
  const decodeInFlightRef = useRef(false);
  const pendingFrameRef = useRef(null);
  const decoderRef = useRef(null);
  const decoderConfigRef = useRef(null);
  const decodeMetaRef = useRef(new Map());
  const waitingForKeyframeRef = useRef(true);
  const lastH264SeqRef = useRef(null);
  const lastH264TimestampRef = useRef(null);
  const mediaSourceRef = useRef(null);
  const sourceBufferRef = useRef(null);
  const sourceBufferMimeRef = useRef('');
  const sourceOpenRef = useRef(false);
  const mseObjectUrlRef = useRef('');
  const segmentQueueRef = useRef([]);
  const videoModeRef = useRef(false);
  const currentDetectionsRef = useRef([]);
  const detectionsDecayRef = useRef(0);
  const sourceResolutionRef = useRef({ w: 1920, h: 1080 });
  const frameBufferRef = useRef({ image: null, detections: [], dirty: false });
  const isMediaSourceInitialized = useRef(false);
  const reconnectAttemptsRef = useRef(0);
  const reconnectingTimerRef = useRef(null);      // Delay before showing "Reconnecting" indicator
  const hasFrameRef = useRef(false);              // Mirror of hasFrame state (ref for stable callbacks)
  const connectWsRef = useRef(null);              // Always points to latest connectWS

  const [hasFrame, setHasFrame] = useState(false);
  const [streamMode, setStreamMode] = useState('connecting');  // connecting | ws | http | reconnecting | error
  const [isHardwareAlert, setIsHardwareAlert] = useState(false);
  const alertTimeoutRef = useRef(null);

  const updateCameraStore = useVmsStore(state => state.updateCamera);

  // Sync internal streamMode with global status to update the sidebar dot and remove global overlay
  useEffect(() => {
    if (streamMode === 'ws' || streamMode === 'http') {
      updateCameraStore({ id: cameraId, status: 'online' });
    } else if (streamMode === 'error') {
      updateCameraStore({ id: cameraId, status: 'error' });
    }
  }, [streamMode, cameraId, updateCameraStore]);

  const canUseWebCodecs = typeof window !== 'undefined' && 'VideoDecoder' in window;
  const canUseMse = typeof window !== 'undefined' && 'MediaSource' in window;

  // Keep hasFrameRef in sync with state so callbacks can read it without deps
  useEffect(() => { hasFrameRef.current = hasFrame; }, [hasFrame]);

  const renderLoop = useCallback(() => {
    if (!mountedRef.current) return;
    const canvas = canvasRef.current;
    const buf = frameBufferRef.current;
    if (canvas && videoModeRef.current) {
        const ctx = canvas.getContext('2d');
        if (canvas.width !== canvas.clientWidth || canvas.height !== canvas.clientHeight) {
            canvas.width = canvas.clientWidth;
            canvas.height = canvas.clientHeight;
        }
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        drawDetections(ctx, currentDetectionsRef.current, canvas.width, canvas.height, sourceResolutionRef.current.w, sourceResolutionRef.current.h);
    } else if (canvas && buf.dirty && buf.image) {
        const ctx = canvas.getContext('2d');
        if (canvas.width !== canvas.clientWidth || canvas.height !== canvas.clientHeight) {
            canvas.width = canvas.clientWidth;
            canvas.height = canvas.clientHeight;
        }
        ctx.drawImage(buf.image, 0, 0, canvas.width, canvas.height);
        drawDetections(ctx, buf.detections, canvas.width, canvas.height, sourceResolutionRef.current.w, sourceResolutionRef.current.h);
        buf.dirty = false;
    }
    rafRef.current = requestAnimationFrame(renderLoop);
  }, []);

  const pushFrame = useCallback((imageBitmap, detections) => {
    const buf = frameBufferRef.current;
    videoModeRef.current = false;

    // Persistence Logic for pushFrame
    if (!detections || detections.length === 0) {
      if (detectionsDecayRef.current < 15) {
        detectionsDecayRef.current++;
        detections = currentDetectionsRef.current;
      } else {
        currentDetectionsRef.current = [];
      }
    } else {
      detectionsDecayRef.current = 0;
      currentDetectionsRef.current = detections;
    }

    if (buf.image && buf.image !== imageBitmap && buf.image.close) buf.image.close();
    buf.image = imageBitmap;
    buf.detections = detections || [];
    buf.dirty = true;
    setHasFrame(h => h ? h : true);
  }, []);

  const scheduleFrameDecode = useCallback(async (blob, detections) => {
    pendingFrameRef.current = { blob, detections };
    if (decodeInFlightRef.current) return;
    decodeInFlightRef.current = true;
    while (mountedRef.current && pendingFrameRef.current) {
      const current = pendingFrameRef.current;
      pendingFrameRef.current = null;
      try {
        const bitmap = await createImageBitmap(current.blob);
        if (!mountedRef.current) { bitmap.close(); break; }
        pushFrame(bitmap, current.detections || []);
      } catch {}
    }
    decodeInFlightRef.current = false;
  }, [pushFrame]);

  const ensureH264Decoder = useCallback((metadata) => {
    if (!canUseWebCodecs) return null;
    const codec = metadata?.codec || 'avc1.42E01E';
    const configKey = `${codec}:annexb`;
    if (!decoderRef.current || decoderConfigRef.current !== configKey) {
      if (decoderRef.current) try { decoderRef.current.close(); } catch {}
      const decoder = new VideoDecoder({
        output: (frame) => {
          const detections = decodeMetaRef.current.get(frame.timestamp) || [];
          decodeMetaRef.current.delete(frame.timestamp);
          pushFrame(frame, detections);
        },
        error: () => { /* decoder error — will recover on next keyframe */ }
      });
      decoder.configure({ codec, optimizeForLatency: true });
      decoderRef.current = decoder;
      decoderConfigRef.current = configKey;
      waitingForKeyframeRef.current = true;
    }
    return decoderRef.current;
  }, [canUseWebCodecs, pushFrame]);

  const resetH264Decoder = useCallback(() => {
    if (decoderRef.current) try { decoderRef.current.close(); } catch {}
    decoderRef.current = null;
    decoderConfigRef.current = null;
    decodeMetaRef.current.clear();
    waitingForKeyframeRef.current = true;
    detectionsDecayRef.current = 0;
    currentDetectionsRef.current = [];
  }, []);

  const cleanupMediaSource = useCallback(() => {
    segmentQueueRef.current = [];
    sourceOpenRef.current = false;
    sourceBufferRef.current = null;
    if (videoRef.current) {
        try { videoRef.current.pause(); } catch {}
        videoRef.current.removeAttribute('src');
        try { videoRef.current.load(); } catch {}
    }
    if (mseObjectUrlRef.current) { URL.revokeObjectURL(mseObjectUrlRef.current); mseObjectUrlRef.current = ''; }
    mediaSourceRef.current = null;
    videoModeRef.current = false;
    detectionsDecayRef.current = 0;
    currentDetectionsRef.current = [];
    isMediaSourceInitialized.current = false;
  }, []);

  const drainSegmentQueue = useCallback(() => {
    const sb = sourceBufferRef.current;
    if (!sb || sb.updating || segmentQueueRef.current.length === 0) return;
    const next = segmentQueueRef.current.shift();
    if (!next) return;

    // Persistence Logic for FMP4 stream
    if (next.objects && next.objects.length > 0) {
        currentDetectionsRef.current = next.objects;
        detectionsDecayRef.current = 0;
    } else {
        if (detectionsDecayRef.current < 15) {
            detectionsDecayRef.current++;
        } else {
            currentDetectionsRef.current = [];
        }
    }

    try {
      sb.appendBuffer(next.payload);
      videoModeRef.current = true;
      setHasFrame(h => h ? h : true);
    } catch {
      cleanupMediaSource();
    }
  }, [cleanupMediaSource]);

  const setupMediaSource = useCallback((mime) => {
    if (!canUseMse || !videoRef.current) return false;
    cleanupMediaSource();
    const ms = new MediaSource();
    mediaSourceRef.current = ms;
    ms.addEventListener('sourceopen', () => {
      try {
        const sb = ms.addSourceBuffer(mime);
        sb.mode = 'segments';
        sb.addEventListener('updateend', () => {
            const v = videoRef.current;
            if (v && v.buffered.length > 0) {
                const end = v.buffered.end(v.buffered.length - 1);
                const delay = end - v.currentTime;
                // Use smoother playbackRate adjustments instead of sudden jumps (which cause visual lag/stuttering)
                if (delay > 2.0) {
                    // Only jump if we are severely behind (e.g. > 2 seconds)
                    v.currentTime = end - 0.1;
                    v.playbackRate = 1.0;
                } else if (delay > 0.8) {
                    v.playbackRate = 1.25; // Smooth catch-up
                } else if (delay > 0.4) {
                    v.playbackRate = 1.1; // Slight catch-up
                } else {
                    v.playbackRate = 1.0; // Normal
                }
            }
            if (v?.paused) v.play().catch(()=>{});

            try {
                if (v && v.currentTime > 10 && !sb.updating && (!v.lastCleanTime || v.currentTime - v.lastCleanTime > 5)) {
                    v.lastCleanTime = v.currentTime;
                    // Safely remove old buffer
                    try { sb.remove(0, v.currentTime - 4); } catch {}
                } else {
                    drainSegmentQueue();
                }
            } catch { drainSegmentQueue(); }
        });
        sourceBufferRef.current = sb;
        sourceOpenRef.current = true;
        drainSegmentQueue();
      } catch {
        cleanupMediaSource();
      }
    }, { once: true });
    mseObjectUrlRef.current = URL.createObjectURL(ms);
    videoRef.current.src = mseObjectUrlRef.current;
    return true;
  }, [canUseMse, cleanupMediaSource, drainSegmentQueue]);

  // ─── WebSocket connection (STABLE — no hasFrame/state in deps) ───
  const connectWS = useCallback(async () => {
    if (!mountedRef.current) return;

    // Cancel any pending reconnect to prevent overlapping timers
    clearTimeout(reconnectTimerRef.current);
    reconnectTimerRef.current = null;

    // Guard: if current WS is still alive, don't create a new one
    const existing = wsRef.current;
    if (existing && (existing.readyState === WebSocket.CONNECTING || existing.readyState === WebSocket.OPEN)) {
      return;
    }

    // Kill stale WS: null handlers FIRST to prevent its onclose from scheduling a reconnect
    if (existing) {
      existing.onopen = null;
      existing.onmessage = null;
      existing.onerror = null;
      existing.onclose = null;
      if (existing.pingInterval) clearInterval(existing.pingInterval);
      try { existing.close(); } catch {}
      wsRef.current = null;
    }

    const wsUrl = await resolveWsUrl();
    // Re-check mount after async gap (resolveWsUrl is cached so this is a microtask)
    if (!mountedRef.current) return;

    let token = null;
    try {
      const res = await apiClient.request('/api/ws/ticket', { method: 'POST' });
      if (res?.success && res?.data?.ticket) {
        token = res.data.ticket;
        console.log('[WS] CameraFeed ticket retrieved successfully');
      } else {
        console.warn('[WS] CameraFeed failed to retrieve ticket:', res);
      }
    } catch (err) {
      console.warn('[WS] CameraFeed ticket request error:', err);
    }

    if (!mountedRef.current) return;

    console.log('[WS] URL:', wsUrl, '| camera:', cameraId, new Date().toISOString());
    const ws = new WebSocket(wsUrl);
    ws.binaryType = 'arraybuffer';
    wsRef.current = ws;

    ws.onopen = () => {
      if (!mountedRef.current) { ws.close(); return; }
      if (token) {
        try { ws.send(JSON.stringify({ type: 'AUTH', token })); } catch {}
      } else {
        // Unauthenticated mode, just send subscription
        ws.send(JSON.stringify({ camera_id: cameraId }));
      }
      ws.pingInterval = setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: 'ping' }));
        }
      }, 20000);
    };

    ws.onmessage = (e) => {
      if (!mountedRef.current) return;

      if (typeof e.data === 'string') {
        try {
          const msg = JSON.parse(e.data);
          // Wait for AUTH_OK before subscribing
          if (msg.type === 'AUTH_OK') {
              ws.send(JSON.stringify({ camera_id: cameraId }));
              return;
          }
          if (msg.type === 'event') {
            setIsHardwareAlert(true);
            if (alertTimeoutRef.current) clearTimeout(alertTimeoutRef.current);
            alertTimeoutRef.current = setTimeout(() => setIsHardwareAlert(false), 4000);
          }
        } catch {}
        return;
      }

      const frame = parseStreamFrame(e.data);
      if (!frame) return;
      if (frame.metadata) {
          if (frame.metadata.width) sourceResolutionRef.current.w = frame.metadata.width;
          if (frame.metadata.height) sourceResolutionRef.current.h = frame.metadata.height;
      }
      if (!wsFrameReceived.current) {
        wsFrameReceived.current = true;
        reconnectAttemptsRef.current = 0; // Reset backoff only after first real frame
        clearTimeout(reconnectingTimerRef.current);
        setStreamMode(s => s === 'ws' ? s : 'ws');
      }

      if (frame.frameType === FRAME_TYPE_H264) {
          const decoder = ensureH264Decoder(frame.metadata);
          if (decoder) {
            const ts = Number(frame.metadata?.ts_us || Date.now()*1000);
            decodeMetaRef.current.set(ts, frame.metadata?.objects || []);
            if (decodeMetaRef.current.size > 200) {
              const cutoff = ts - 5000000;
              for (const [k] of decodeMetaRef.current) { if (k < cutoff) decodeMetaRef.current.delete(k); else break; }
            }
            decoder.decode(new EncodedVideoChunk({ type: frame.metadata?.keyframe ? 'key' : 'delta', timestamp: ts, data: frame.payload }));
          }
      } else if (frame.frameType === FRAME_TYPE_FMP4) {
          const metadata = frame.metadata || {};
          const isInit = metadata.segment_type === 'init' || (frame.flags & 0x01);
          if (isInit) {
            if (!isMediaSourceInitialized.current) {
              setupMediaSource(metadata.mime || 'video/mp4; codecs="avc1.42E01E"');
              isMediaSourceInitialized.current = true;
            }
          }
          if (segmentQueueRef.current.length >= MAX_SEGMENT_QUEUE) {
            segmentQueueRef.current.shift();
          }
          segmentQueueRef.current.push({ payload: frame.payload, objects: metadata.objects || [] });
          drainSegmentQueue();
      } else if (frame.frameType === FRAME_TYPE_JPEG) {
          scheduleFrameDecode(new Blob([frame.payload], { type: 'image/jpeg' }), frame.metadata?.objects || []);
      }
    };

    ws.onerror = (err) => {
      console.error('[WS] error — camera:', cameraId, 'URL:', wsUrl, err);
      // onclose fires after onerror — reconnect handled there
    };

    ws.onclose = (ev) => {
      if (ws.pingInterval) clearInterval(ws.pingInterval);
      console.log('[WS] closed', cameraId, 'code:', ev.code, 'reason:', ev.reason,
        'attempts:', reconnectAttemptsRef.current, new Date().toISOString());
      // Only reconnect if THIS ws is still the current one (prevents stale closures)
      if (!mountedRef.current || wsRef.current !== ws) return;
      wsRef.current = null;
      wsFrameReceived.current = false;
      const attempt = reconnectAttemptsRef.current++;
      // Server-initiated close (1001=going away, 1006=abnormal): use longer base delay
      const base = (ev.code === 1001 || ev.code === 1006) ? WS_RECONNECT_MAX_MS / 2 : WS_RECONNECT_BASE_MS;
      const delay = Math.min(base * Math.pow(2, Math.min(attempt, 4)), WS_RECONNECT_MAX_MS);
      if (!hasFrameRef.current) {
        setStreamMode('connecting');
      } else {
        clearTimeout(reconnectingTimerRef.current);
        reconnectingTimerRef.current = setTimeout(() => {
          if (mountedRef.current) setStreamMode('reconnecting');
        }, 2000);
      }
      reconnectTimerRef.current = setTimeout(() => connectWsRef.current?.(), delay);
    };
  // CRITICAL: NO hasFrame, NO streamMode in deps — use refs instead to keep this stable
  }, [cameraId, drainSegmentQueue, ensureH264Decoder, scheduleFrameDecode, setupMediaSource]);

  // Keep ref always pointing to latest connectWS (same pattern as useWebSocket)
  useEffect(() => { connectWsRef.current = connectWS; }, [connectWS]);

  const startHttpPolling = useCallback(() => {
    let consecutiveErrors = 0;
    const poll = async () => {
      if (!mountedRef.current || wsFrameReceived.current) return;
      try {
        const res = await fetch(`${apiClient.getBaseUrl()}/api/cameras/${cameraId}/frame?t=${Date.now()}`, {
          credentials: 'include'
        });
        if (res.ok) {
            consecutiveErrors = 0;
            let detections = currentDetectionsRef.current;
            const metaStr = res.headers.get('X-Metadata');
            if (metaStr && metaStr !== '[]') {
                try { detections = JSON.parse(metaStr); } catch {}
            }
            const blob = await res.blob();
            scheduleFrameDecode(blob, detections);
            setStreamMode('http');
            httpTimerRef.current = setTimeout(poll, HTTP_FRAME_INTERVAL);
        } else if (res.status === 503) {
            // Camera is starting up - carry on with "connecting" status but don't count as hard error
            if (!hasFrameRef.current) setStreamMode('connecting');
            httpTimerRef.current = setTimeout(poll, 1000); 
        } else {
            consecutiveErrors++;
            const backoff = Math.min(2000 * consecutiveErrors, 10000);
            if (consecutiveErrors > 5) setStreamMode('error');
            httpTimerRef.current = setTimeout(poll, backoff);
        }
      } catch {
        consecutiveErrors++;
        const backoff = Math.min(2000 * consecutiveErrors, 10000);
        if (consecutiveErrors > 5) setStreamMode('error');
        httpTimerRef.current = setTimeout(poll, backoff);
      }
    };
    setStreamMode('http');
    poll();
  }, [cameraId, scheduleFrameDecode]);

  // ─── Main lifecycle: connect once, cleanup fully ───
  useEffect(() => {
    mountedRef.current = true;
    rafRef.current = requestAnimationFrame(renderLoop);
    connectWS();
    fallbackTimerRef.current = setTimeout(() => { if (!wsFrameReceived.current && mountedRef.current) startHttpPolling(); }, WS_FALLBACK_TIMEOUT);
    return () => {
      mountedRef.current = false;
      cancelAnimationFrame(rafRef.current);

      // Cancel all pending timers FIRST
      clearTimeout(reconnectTimerRef.current);
      reconnectTimerRef.current = null;
      clearTimeout(reconnectingTimerRef.current);
      clearTimeout(fallbackTimerRef.current);
      clearTimeout(httpTimerRef.current);

      // Kill WS: null handlers to prevent stale onclose from scheduling reconnects
      const ws = wsRef.current;
      if (ws) {
        ws.onopen = null;
        ws.onmessage = null;
        ws.onerror = null;
        ws.onclose = null;
        if (ws.pingInterval) clearInterval(ws.pingInterval);
        try { ws.close(); } catch {}
        wsRef.current = null;
      }

      resetH264Decoder();
      cleanupMediaSource();
    };
  }, [connectWS, startHttpPolling, renderLoop, resetH264Decoder, cleanupMediaSource]);

  return (
    <div className={`cam-feed ${isHardwareAlert ? 'hardware-alert' : ''}`} style={{ position:'relative', width:'100%', height:'100%', background:'#000', overflow:'hidden' }}>
      <video ref={videoRef} autoPlay muted playsInline style={{ width:'100%', height:'100%', objectFit:'cover', position:'absolute', inset:0, opacity: hasFrame && videoModeRef.current ? 1 : 0, zIndex:1 }} />
      <canvas ref={canvasRef} style={{ pointerEvents: 'none', width:'100%', height:'100%', objectFit:'cover', position:'absolute', inset:0, opacity: hasFrame ? 1 : 0.2, zIndex:2 }} />
      {isHardwareAlert && <div className="alert-overlay"></div>}
      {!hasFrame && (
        <div style={{ position:'absolute', inset:0, display:'flex', alignItems:'center', justifyContent:'center', color: streamMode === 'error' ? 'var(--danger, #ff3060)' : 'var(--text-dim)', fontSize:'10px', zIndex:3 }}>
          {streamMode === 'error'
            ? <><span style={{marginRight:4}}>&#9888;</span> Connection lost</>
            : <><span className="ai-dot-spin"></span> Connecting...</>
          }
        </div>
      )}
      {hasFrame && streamMode === 'reconnecting' && (
        <div style={{ position:'absolute', top:4, right:4, zIndex:10, fontSize:'8px', color:'#ffa500', background:'rgba(0,0,0,0.5)', padding:'2px 6px', borderRadius:3 }}>
          <span className="ai-dot-spin"></span> Reconnecting...
        </div>
      )}
      {showControls && (
         <div style={{ position:'absolute', bottom:4, left:4, zIndex:10, fontSize:'8px', color: streamMode === 'error' ? '#ff3060' : 'rgba(255,255,255,0.4)', background:'rgba(0,0,0,0.3)', padding:'1px 4px', borderRadius:2 }}>
            {streamMode.toUpperCase()}
         </div>
      )}
    </div>
  );
});

export default CameraFeed;
