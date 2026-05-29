import { API_BASE_URL, WS_BASE_URL } from './runtimeUrls';

// Cached result — WS port is static for the backend's lifetime
let _resolvedUrl = null;
let _inflight = null;

/**
 * Fetch the WebSocket URL from /api/system/streaming-config.
 * Result is cached for the session — the backend port never changes at runtime.
 */
export async function resolveWsUrl() {
  if (_resolvedUrl) return _resolvedUrl;
  if (_inflight) return _inflight;

  _inflight = (async () => {
    // Ask the backend for the actual bound WS port. This works in both dev
    // mode (via Vite's /api proxy) and direct/backend-served mode.
    try {
      const headers = { 'Content-Type': 'application/json' };

      const res = await fetch(`${API_BASE_URL}/api/system/streaming-config`, {
        headers,
        credentials: 'include',
      });
      if (res.ok) {
        const json = await res.json();
        const port = json?.data?.websocket_port;
        const available = json?.data?.websocket_available;
        if (available && port > 0) {
          const apiHost = API_BASE_URL
            ? new URL(API_BASE_URL, window.location.origin).hostname
            : window.location.hostname;
          const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
          const url = `${wsProtocol}//${apiHost}:${port}/ws`;
          _resolvedUrl = url;
          return url;
        }
        console.warn('[streamingConfig] streaming-config returned invalid port:', json);
      } else {
        console.warn('[streamingConfig] streaming-config HTTP error:', res.status);
      }
    } catch (e) {
      console.warn('[streamingConfig] Failed to fetch streaming-config:', e.message);
    }

    // Fallback — uses WS_BASE_URL derived from VITE_API_URL
    const fallback = WS_BASE_URL;
    console.warn('[streamingConfig] Using fallback WS URL:', fallback);
    _resolvedUrl = fallback;
    return fallback;
  })();

  return _inflight;
}

// Reset cache on logout so next login re-fetches (port won't change, but keeps state clean)
window.addEventListener('vms_logout', () => {
  _resolvedUrl = null;
  _inflight = null;
});
