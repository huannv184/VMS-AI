import { API_BASE_URL, WS_BASE_URL } from './runtimeUrls';

// Cached result — WS port is static for the backend's lifetime
let _resolvedUrl = null;
let _inflight = null;

/**
 * Determine whether we are running behind the Vite dev server proxy.
 * When API_BASE_URL is empty (relative), all /api and /ws requests go through
 * the Vite proxy on the same origin, so we must use same-origin WS too.
 */
const isProxiedDevMode = () => !API_BASE_URL;

/**
 * Build same-origin WebSocket URL (goes through Vite dev proxy).
 */
const getSameOriginWsUrl = () => {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${window.location.host}/ws`;
};

/**
 * Fetch the WebSocket URL from /api/system/streaming-config.
 * Result is cached for the session — the backend port never changes at runtime.
 *
 * In dev mode (Vite proxy), we ALWAYS use same-origin /ws so the proxy handles
 * routing. This avoids CORS issues and ensures cookies are sent correctly.
 *
 * In production (API_BASE_URL is an absolute URL), we resolve the actual
 * port from the backend and connect directly.
 */
export async function resolveWsUrl() {
  if (_resolvedUrl) return _resolvedUrl;
  if (_inflight) return _inflight;

  _inflight = (async () => {
    // Dev mode: avoid Vite dev proxy for WS because it causes 1006 connection drops when backend 
    // pumps rapid binary FMP4 frames right after initialization. Connect directly to 8083.
    if (isProxiedDevMode()) {
      const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      const url = `${protocol}//${window.location.hostname}:8083/ws`;
      console.log('[streamingConfig] Dev mode — bypassing Vite proxy. Using direct WS URL:', url);
      _resolvedUrl = url;
      return url;
    }

    // Production / direct mode: ask backend for actual WS port
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
          const apiHost = new URL(API_BASE_URL, window.location.origin).hostname;
          const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
          const url = `${wsProtocol}//${apiHost}:${port}/ws`;
          console.log('[streamingConfig] WS URL resolved from backend:', url);
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
