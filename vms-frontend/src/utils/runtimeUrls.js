// Runtime URL resolution for API + WebSocket. Both are build-time settings
// baked from Vite env (`import.meta.env`) — set them in `.env.local` (gitignored)
// for local overrides or in CI per-deploy. See `.env.example` for the operator
// knobs and when each one matters.
//
// Deployment shapes:
//   1. Dev with Vite proxy + same-host backend (default):
//        VITE_API_URL unset → relative paths, Vite proxy forwards /api/*
//        VITE_WS_URL  unset → ws://<hostname>:8083/ws (skips Vite proxy
//                              because binary FMP4 frames get truncated)
//   2. Prod, FE + backend on the same origin:
//        VITE_API_URL unset → relative paths work
//        VITE_WS_URL  unset → derived from window.location (same host, /ws)
//   3. Prod, FE on a different host (CDN, static bucket, separate domain):
//        VITE_API_URL = https://api.example.com
//        VITE_WS_URL  unset → auto-derived from VITE_API_URL (wss:// + /ws)
//                            OR set explicitly if WS is on a different host

const trimTrailingSlash = (value = '') => value.replace(/\/+$/, '');

const resolveApiBaseUrl = () => {
  // PR-4 (2026-05-25): wire VITE_API_URL. Default to empty so the standard
  // Vite-proxy / same-origin deployment keeps working unchanged. Set the env
  // var only when the FE bundle is served from a different host than the
  // backend API — otherwise relative paths + the proxy / same-origin
  // behaviour is what you want (no CORS, no SameSite=Strict cookie issue).
  return trimTrailingSlash(import.meta.env.VITE_API_URL || '');
};

const withWsPath = (value = '') => {
  if (!value) return '';

  try {
    const url = new URL(value);
    if (!url.pathname || url.pathname === '/') {
      url.pathname = '/ws';
    }
    return trimTrailingSlash(url.toString());
  } catch {
    const normalized = trimTrailingSlash(value);
    return normalized.endsWith('/ws') ? normalized : `${normalized}/ws`;
  }
};

const deriveWsUrlFromApi = (apiBaseUrl = '') => {
  if (!apiBaseUrl) return '';

  try {
    const url = new URL(apiBaseUrl);
    url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
    url.pathname = '/ws';
    url.search = '';
    url.hash = '';
    return trimTrailingSlash(url.toString());
  } catch {
    return '';
  }
};

const getDevHostWsUrl = () => {
  // Dev fallback: skip the Vite proxy (it truncates binary FMP4 frames) and
  // hit the backend WS port directly on the same hostname the page came from.
  // Only fires when neither VITE_WS_URL nor VITE_API_URL is set.
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${window.location.hostname}:8083/ws`;
};

export const API_BASE_URL = resolveApiBaseUrl();

export const WS_BASE_URL = (() => {
  // Fallback chain (first non-empty wins):
  //   1. VITE_WS_URL          — operator explicitly set WS host
  //   2. VITE_API_URL         — derive WS from the API URL (same host, /ws)
  //   3. Dev host fallback    — hostname:8083/ws (legacy default)
  //
  // PR-4 (2026-05-25) added step 2 so split-host deploys don't have to set
  // both env vars when WS lives on the same host as the API.

  const explicitWsUrl = trimTrailingSlash(import.meta.env.VITE_WS_URL || '');
  if (explicitWsUrl) {
    return withWsPath(explicitWsUrl);
  }

  const derivedFromApi = deriveWsUrlFromApi(API_BASE_URL);
  if (derivedFromApi) {
    return derivedFromApi;
  }

  return getDevHostWsUrl();
})();
