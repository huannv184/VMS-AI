const trimTrailingSlash = (value = '') => value.replace(/\/+$/, '');

const resolveApiBaseUrl = () => {
  // PR-4 (2026-05-25): wire VITE_API_URL. Empty default preserves the
  // standard Vite-proxy / same-origin shape. Set the env var only when the
  // FE bundle is served from a different host than the backend.
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

export const API_BASE_URL = resolveApiBaseUrl();

export const WS_BASE_URL = (() => {
  // Fallback chain: VITE_WS_URL → VITE_API_URL-derived → dev hostname:8083.
  // PR-4 (2026-05-25) added the middle step so split-host deploys don't
  // have to set both env vars when WS lives on the same host as the API.
  const explicitWsUrl = trimTrailingSlash(import.meta.env.VITE_WS_URL || '');
  if (explicitWsUrl) {
    return withWsPath(explicitWsUrl);
  }

  if (API_BASE_URL) {
    try {
      const url = new URL(API_BASE_URL);
      url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
      url.pathname = '/ws';
      url.search = '';
      url.hash = '';
      return trimTrailingSlash(url.toString());
    } catch {
      // Fall through to dev fallback below.
    }
  }

  // Dev fallback — skip the Vite proxy (truncates binary FMP4 frames) and
  // hit backend WS port directly on the same hostname the page came from.
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${window.location.hostname}:8083/ws`;
})();
