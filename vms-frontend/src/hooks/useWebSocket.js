import { useEffect, useRef, useCallback } from 'react';
import useVmsStore from '../store/useVmsStore';
import cameraMetaBus from '../utils/cameraMetaBus';
import { resolveWsUrl } from '../utils/streamingConfig';
import { API_BASE_URL } from '../utils/runtimeUrls';
import apiClient from '../api/apiClient';

// Backend emits Unix seconds (std::chrono::seconds). new Date(seconds) treats
// the number as milliseconds and renders 1970 timestamps; multiply by 1000.
// Tolerates already-ms values (anything > 9999999999 is past year 2286 in s).
const normalizeTimestamp = (v) => (v > 9999999999 ? v : v * 1000);

const useWebSocket = () => {
  const socket        = useRef(null);
  const reconnectRef  = useRef(null);
  const connectRef    = useRef(null);
  const authRejected  = useRef(false);
  const alarmIdCounter= useRef(0);

  const addFace           = useVmsStore((s) => s.addFace);
  const addAlarm          = useVmsStore((s) => s.addAlarm);
  const setCounts         = useVmsStore((s) => s.setCounts);
  const updateLineCrossing= useVmsStore((s) => s.updateLineCrossing);
  const setIsConnected    = useVmsStore((s) => s.setIsConnected);

  const disconnect = useCallback((permanent = false) => {
    clearTimeout(reconnectRef.current);
    if (permanent) authRejected.current = true;
    if (socket.current) {
      socket.current.onclose = null;
      socket.current.onerror = null;
      if (
        socket.current.readyState === WebSocket.OPEN ||
        socket.current.readyState === WebSocket.CONNECTING
      ) {
        socket.current.close();
      }
      socket.current = null;
    }
    setIsConnected(false);
  }, [setIsConnected]);

  const connect = useCallback(async () => {
    // Fetch a short-lived WS ticket using the same authenticated context as the
    // REST API. Keep the access token in storage; the browser may still need it
    // after reload when the session cookie is unavailable in plain HTTP dev mode.
    let token = null;
    try {
      const accessToken = apiClient.getToken?.();
      const authHeader = accessToken ? `Bearer ${accessToken}` : '';
      const res = await fetch(`${API_BASE_URL}/api/ws/ticket`, {
        method: 'POST',
        credentials: 'include',
        headers: {
          'Content-Type': 'application/json',
          ...(authHeader ? { 'Authorization': authHeader } : {}),
        },
      });
      const json = await res.json();
      if (json?.success && json?.data?.ticket) {
        token = json.data.ticket;
      } else {
        throw new Error(json?.error?.message || 'No ticket in response');
      }
    } catch (e) {
      console.warn('[WS] Failed to get ticket, possibly unauthenticated:', e.message);
      authRejected.current = true;
      disconnect();
      return;
    }

    authRejected.current = false;

    if (
      socket.current &&
      (socket.current.readyState === WebSocket.OPEN || socket.current.readyState === WebSocket.CONNECTING)
    ) {
      return;
    }

    disconnect();

    const wsUrl = await resolveWsUrl();
    console.log('[WS] URL:', wsUrl);
    socket.current = new WebSocket(wsUrl);

    const handleMessage = (msg) => {
      const type    = msg.type;
      const payload = msg.payload || msg.event || msg;
      const API_BASE = '';

      switch (type) {
        case 'EVENT_FACE':
        case 'face':
          addFace({
            id:         payload.id || payload.track_id,
            name:       payload.person_name || 'Người lạ',
            type:       payload.label || 'UNKNOWN',
            confidence: payload.confidence || 0,
            time:       payload.timestamp
              ? new Date(normalizeTimestamp(payload.timestamp)).toLocaleTimeString('vi-VN')
              : new Date().toLocaleTimeString('vi-VN'),
            avatar: payload.face_image_path
              ? `${API_BASE}${payload.face_image_path}` : null,
          });
          break;

        case 'EVENT_ALARM':
        case 'detection':
        case 'alert':
        case 'event':
          addAlarm({
            // Date.now() ties at sub-ms when bursty events arrive in the same tick.
            // Combine with a per-message counter to keep React keys unique.
            id:       payload.id || `${Date.now()}-${++alarmIdCounter.current}`,
            time:     payload.timestamp
              ? new Date(normalizeTimestamp(payload.timestamp)).toLocaleTimeString('vi-VN')
              : new Date().toLocaleTimeString('vi-VN'),
            type:     payload.label || payload.event_type || 'AI',
            desc:     payload.message || payload.description || 'Phát hiện sự kiện AI',
            camera:   payload.camera_name || `CAM-${payload.camera_id || '01'}`,
            severity: payload.severity || 'low',
            status:   'new',
            img: payload.snapshot_url
              ? `${API_BASE}${payload.snapshot_url}` : null,
          });
          break;

        case 'STATS_UPDATE':
        case 'stats':
          setCounts({
            people:        payload.people    || 0,
            vehicles:      payload.vehicles  || 0,
            alerts:        payload.alerts    || 0,
            ppeViolations: payload.ppe       || 0,
          });
          break;

        case 'METADATA':
        case 'camera_metadata':
          // PERF-OPT: Route through EventEmitter, bypass React store entirely
          if (payload?.camera_id) {
            cameraMetaBus.emit(
              payload.camera_id,
              payload.detections || payload.objects || []
            );
          }
          break;

        case 'LINE_CROSSING':
          updateLineCrossing(payload.gate, payload.data);
          break;

        case 'AUTH_FAILED':
        case 'auth_failed':
          authRejected.current = true;
          apiClient.clearToken();
          window.dispatchEvent(new Event('vms_logout'));
          disconnect(true);
          break;

        case 'PONG':
          break;

        default:
          break;
      }
    };

    socket.current.onopen = () => {
      try {
        socket.current?.send(JSON.stringify({ type: 'AUTH', token }));
      } catch (e) {
        console.warn('WS: AUTH send failed', e);
      }
      setIsConnected(true);
      clearTimeout(reconnectRef.current);
    };

    socket.current.onmessage = (event) => {
      if (typeof event.data === 'string') {
        try {
          const data = JSON.parse(event.data);
          handleMessage(data);
        } catch (e) {
          console.warn('[WS] Malformed JSON message — dropping:', e.message);
        }
      }
    };

    socket.current.onclose = (event) => {
      console.warn('[WS] closed — code:', event.code, 'reason:', event.reason || '(none)');
      setIsConnected(false);
      socket.current = null;
      // 1008 = Policy Violation (auth rejected); also check authRejected flag set by AUTH_FAILED message
      const authDenied = event.code === 1008 || authRejected.current;
      if (!authDenied && !authRejected.current) {
        console.log('[WS] scheduling reconnect in 5s');
        reconnectRef.current = setTimeout(() => connectRef.current?.(), 5000);
      }
    };

    socket.current.onerror = (err) => {
      console.error('[WS] error — URL was:', wsUrl, err);
      setIsConnected(false);
    };
  }, [addFace, addAlarm, disconnect, setCounts, updateLineCrossing, setIsConnected]);

  useEffect(() => {
    connectRef.current = connect;
  }, [connect]);

  useEffect(() => {
    connect();

    const pingInterval = setInterval(() => {
      if (socket.current?.readyState === WebSocket.OPEN) {
        socket.current.send(JSON.stringify({ type: 'ping' }));
      }
    }, 30000);

    return () => {
      clearInterval(pingInterval);
      disconnect();
    };
  }, [connect, disconnect]);

  useEffect(() => {
    const handleAuthChanged = () => {
      clearTimeout(reconnectRef.current);
      // We assume vms_auth_changed indicates log in or out. 
      // If we are logged out, authRejected.current can be checked or just reset.
      authRejected.current = false;
      connectRef.current?.();
    };

    window.addEventListener('vms_auth_changed', handleAuthChanged);
    return () => window.removeEventListener('vms_auth_changed', handleAuthChanged);
  }, [disconnect]);

  return { isConnected: useVmsStore.getState().isConnected };
};

export default useWebSocket;
