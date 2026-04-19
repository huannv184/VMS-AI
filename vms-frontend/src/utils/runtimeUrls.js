const trimTrailingSlash = (value = '') => value.replace(/\/+$/, '');

const resolveApiBaseUrl = () => {
  // Always use relative paths when served from a web server or Vite dev server.
  // This ensures the Vite proxy handles /api routing, bypassing CORS and SameSite=Strict cookie issues.
  return '';
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

const getSameOriginWsUrl = () => {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${window.location.host}/ws`;
};

export const API_BASE_URL = resolveApiBaseUrl();

export const WS_BASE_URL = (() => {
  // Production: use explicit WS URL or derive from API URL
  const explicitWsUrl = trimTrailingSlash(import.meta.env.VITE_WS_URL || '');
  if (explicitWsUrl) {
    return withWsPath(explicitWsUrl);
  }

  // In dev mode, connect directly to backend WS port (8083) to avoid Vite proxy dropping binary FMP4 frames
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${window.location.hostname}:8083/ws`;
})();
