import { API_BASE_URL } from '../utils/runtimeUrls';

const BASE_URL = API_BASE_URL;
// H5: Token is now stored in an HttpOnly cookie set by the backend on login.
// We keep a minimal in-memory flag to know whether the user is authenticated
// (no actual token value is stored in JS-accessible storage).
const TOKEN_KEY = 'vms_token';          // kept for backward compat with useWebSocket
const AUTH_CHANGE_EVENT = 'vms_auth_changed';

let isLoggingOut = false;

// In-memory auth state — no token value, just presence.
// The real token lives in the HttpOnly cookie; we mirror it to localStorage
// ONLY for the WebSocket handshake which sends it as a message payload
// (browsers cannot attach cookies to WS frames natively).
// This is a known limitation; plan to move WS auth to a short-lived ticket endpoint.
let _inMemoryToken = null;

const toHeadersObject = (headers) => {
  if (headers instanceof Headers) {
    return Object.fromEntries(headers.entries());
  }
  return { ...(headers || {}) };
};

const hasHeader = (headers, target) => (
  Object.keys(headers).some((key) => key.toLowerCase() === target.toLowerCase())
);

const getToken = () => _inMemoryToken || localStorage.getItem(TOKEN_KEY);

const dispatchAuthChanged = (token) => {
  window.dispatchEvent(new CustomEvent(AUTH_CHANGE_EVENT, { detail: { token } }));
};

const setToken = (token, localStorageOnly = false) => {
  if (!localStorageOnly) {
    _inMemoryToken = token || null;
  }
  
  if (token) {
    // Keep in localStorage for WS auth ticket retrieval only
    localStorage.setItem(TOKEN_KEY, token);
    localStorage.setItem('vms_auth_status', 'true');
  } else {
    localStorage.removeItem(TOKEN_KEY);
    if (!localStorageOnly) {
      localStorage.removeItem('vms_auth_status');
    }
  }
  
  if (!localStorageOnly) {
    dispatchAuthChanged(token || null);
  }
};

const handleUnauthorized = () => {
  const isAuthenticated = localStorage.getItem('vms_auth_status') === 'true';
  if ((!getToken() && !isAuthenticated) || isLoggingOut) {
    return;
  }
  isLoggingOut = true;
  setToken(null);
  window.dispatchEvent(new Event('vms_logout'));
  setTimeout(() => {
    isLoggingOut = false;
  }, 1000);
};

const buildQuery = (params = {}) => {
  const searchParams = new URLSearchParams();

  Object.entries(params).forEach(([key, value]) => {
    if (value === undefined || value === null || value === '') {
      return;
    }
    if (Array.isArray(value)) {
      value.forEach((entry) => searchParams.append(key, String(entry)));
      return;
    }
    searchParams.append(key, String(value));
  });

  const query = searchParams.toString();
  return query ? `?${query}` : '';
};

const extractFilename = (headers) => {
  const disposition = headers.get('content-disposition') || '';
  const utf8Match = disposition.match(/filename\*\s*=\s*UTF-8''([^;]+)/i);
  if (utf8Match?.[1]) {
    return decodeURIComponent(utf8Match[1]);
  }
  const plainMatch = disposition.match(/filename\s*=\s*"?([^";]+)"?/i);
  return plainMatch?.[1] || null;
};

/**
 * Extract a human-readable error string from the contract error field.
 * Backend returns error as { code: string, message: string } or null.
 * Legacy/fallback: error may still be a plain string.
 */
const extractErrorMessage = (errorField) => {
  if (!errorField) return null;
  if (typeof errorField === 'string') return errorField;
  if (typeof errorField === 'object' && errorField.message) return errorField.message;
  return String(errorField);
};

const normalizeEnvelope = (response, payload, responseType) => {
  // Blob / text responses — pass through as-is (Phase 7: export exception)
  if (responseType === 'blob' || responseType === 'text') {
    const success = response.ok;
    return {
      ok: success,
      success,
      data: success ? payload : null,
      error: success ? null : ((payload && typeof payload === 'object' && !(payload instanceof Blob)) ? extractErrorMessage(payload.error) : payload) || response.statusText || 'Request failed',
      status: response.status,
      headers: response.headers,
      filename: responseType === 'blob' ? extractFilename(response.headers) : null,
      meta: null,
      raw: payload,
    };
  }

  // Contract envelope: { success, data, error, meta }
  if (payload && typeof payload === 'object' && !Array.isArray(payload) && Object.prototype.hasOwnProperty.call(payload, 'success')) {
    const success = Boolean(payload.success);
    const hasData = Object.prototype.hasOwnProperty.call(payload, 'data');

    return {
      ok: success,
      success,
      data: hasData ? payload.data : null,
      error: extractErrorMessage(payload.error),
      status: response.status,
      headers: response.headers,
      filename: null,
      meta: payload.meta || null,
      raw: payload,
    };
  }

  // Fallback: non-contract response (shouldn't happen after migration)
  if (response.ok) {
    return {
      ok: true,
      success: true,
      data: payload,
      error: null,
      status: response.status,
      headers: response.headers,
      filename: null,
      meta: null,
      raw: payload,
    };
  }

  return {
    ok: false,
    success: false,
    data: null,
    error: (payload && typeof payload === 'object' ? extractErrorMessage(payload.error) : payload) || response.statusText || 'Request failed',
    status: response.status,
    headers: response.headers,
    filename: null,
    meta: null,
    raw: payload,
  };
};

const apiClient = {
  getBaseUrl: () => BASE_URL,
  getToken,
  setToken,
  clearToken: (localStorageOnly = false) => setToken(null, localStorageOnly),
  buildQuery,

  request: async (endpoint, options = {}, responseType = 'json') => {
    const token = getToken();
    const url = `${BASE_URL}${endpoint}`;
    const headers = toHeadersObject(options.headers);

    if (token && !hasHeader(headers, 'Authorization')) {
      headers.Authorization = `Bearer ${token}`;
    }

    const body = options.body;
    if (body !== undefined && !(body instanceof FormData) && !(body instanceof Blob) && !hasHeader(headers, 'Content-Type')) {
      headers['Content-Type'] = 'application/json';
    }

    const requestOptions = {
      ...options,
      headers,
      // H5: include HttpOnly cookie on every request (same-origin + CORS with credentials)
      credentials: 'include',
    };

    try {
      const response = await fetch(url, requestOptions);
      let payload = null;

      if (response.status !== 204) {
        const contentType = response.headers.get('content-type') || '';

        if (responseType === 'blob') {
          if (response.ok) {
            payload = await response.blob().catch(() => null);
          } else if (contentType.includes('application/json')) {
            payload = await response.json().catch(() => null);
          } else if (contentType.includes('text/') || contentType.includes('csv')) {
            payload = await response.text().catch(() => null);
          } else {
            payload = await response.blob().catch(() => null);
          }
        } else if (responseType === 'text') {
          payload = await response.text().catch(() => null);
        } else if (contentType.includes('application/json')) {
          payload = await response.json().catch(() => null);
        } else if (contentType.includes('text/') || contentType.includes('csv')) {
          payload = await response.text().catch(() => null);
        } else {
          payload = await response.blob().catch(() => null);
        }
      }

      const result = normalizeEnvelope(response, payload, responseType);

      if (response.status === 401) {
        handleUnauthorized();
      }

      return result;
    } catch (error) {
      return {
        ok: false,
        success: false,
        data: null,
        error: error.message || 'Network error',
        status: 0,
        headers: new Headers(),
        filename: null,
        meta: null,
        raw: null,
      };
    }
  },

  fetch: async (endpoint, options = {}, responseType = 'json') => (
    apiClient.request(endpoint, options, responseType)
  ),

  download: async (endpoint, options = {}) => apiClient.request(endpoint, options, 'blob'),

  saveBlobResponse: (result, fallbackName = 'download.bin') => {
    if (!result?.success || !(result.data instanceof Blob)) {
      return false;
    }

    const url = window.URL.createObjectURL(result.data);
    const anchor = document.createElement('a');
    anchor.style.display = 'none';
    anchor.href = url;
    anchor.download = result.filename || fallbackName;
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    window.URL.revokeObjectURL(url);
    return true;
  },

  normalizeEmbeddedError: (result) => {
    if (result?.success && result.data && typeof result.data === 'object' && result.data.error) {
      const msg = typeof result.data.error === 'string' ? result.data.error : (result.data.error.message || String(result.data.error));
      return {
        ...result,
        ok: false,
        success: false,
        error: msg,
      };
    }
    return result;
  },

  unsupported: (message) => Promise.resolve({
    ok: false,
    success: false,
    data: null,
    error: message,
    status: 501,
    headers: new Headers(),
    filename: null,
    meta: null,
    raw: null,
  }),

  login: async (username, password) => {
    const result = await apiClient.request('/api/auth/login', {
      method: 'POST',
      body: JSON.stringify({ username, password }),
    });

    const token = result.data?.token;
    if (result.success && token) {
      setToken(token);
    }

    return result;
  },

  setup2FA: () => apiClient.request('/api/auth/2fa/setup', { method: 'POST' }),

  enable2FA: (code, secret) => apiClient.request('/api/auth/2fa/enable', {
    method: 'POST',
    body: JSON.stringify({ code, secret }),
  }),

  verify2FA: async (tempToken, code) => {
    const result = await apiClient.request('/api/auth/2fa/verify', {
      method: 'POST',
      body: JSON.stringify({ temp_token: tempToken, code }),
    });

    const token = result.data?.token;
    if (result.success && token) {
      setToken(token);
    }

    return result;
  },

  getCurrentUser: () => apiClient.request('/api/auth/me'),

  getUsers: () => apiClient.request('/api/users'),
  getCameras: () => apiClient.request('/api/cameras'),
  getCamera: (id) => apiClient.request(`/api/cameras/${id}`),
  addCamera: (data) => apiClient.request('/api/cameras', { method: 'POST', body: JSON.stringify(data) }),
  discoverOnvif: () => apiClient.request('/api/system/onvif/discover'),
  startNetworkScan: (data) => apiClient.request('/api/system/network/scan/start', { method: 'POST', body: JSON.stringify(data) }),
  getNetworkScanStatus: (scanId) => apiClient.request(`/api/system/network/scan/status${buildQuery({ scan_id: scanId })}`),
  stopNetworkScan: (scanId) => apiClient.request('/api/system/network/scan/stop', { method: 'POST', body: JSON.stringify({ scan_id: scanId }) }),
  discoverCamera: async (data) => apiClient.normalizeEmbeddedError(await apiClient.request('/api/cameras/discover', {
    method: 'POST',
    body: JSON.stringify(data),
  })),
  updateCamera: (id, data) => apiClient.request(`/api/cameras/${id}`, { method: 'PUT', body: JSON.stringify(data) }),
  deleteCamera: (id) => apiClient.request(`/api/cameras/${id}`, { method: 'DELETE' }),
  importCameras: (data, isCsv = false) => apiClient.request('/api/cameras/import', {
    method: 'POST',
    headers: isCsv ? { 'Content-Type': 'text/csv' } : undefined,
    body: isCsv ? data : JSON.stringify(data),
  }),
  startCamera: (id) => apiClient.request(`/api/cameras/${id}/start`, { method: 'POST' }),
  stopCamera: (id) => apiClient.request(`/api/cameras/${id}/stop`, { method: 'POST' }),
  getCameraStats: (id) => apiClient.request(`/api/cameras/${id}/stats`),
  getCameraMetadata: (id) => apiClient.request(`/api/cameras/${id}/metadata`),
  snapshotCamera: (id) => apiClient.request(`/api/cameras/${id}/snapshot`, { method: 'POST' }),
  ptzCamera: (id, action, value = 0) => apiClient.request(`/api/cameras/${id}/ptz`, {
    method: 'POST',
    body: JSON.stringify({ action, value }),
  }),
  ptzMove: (id, pan, tilt, zoom) => apiClient.request(`/api/ptz/${id}/move`, {
    method: 'POST',
    body: JSON.stringify({ pan, tilt, zoom }),
  }),
  ptzStop: (id) => apiClient.request(`/api/ptz/${id}/stop`, { method: 'POST' }),
  ptzAbsolute: (id, pan, tilt, zoom) => apiClient.request(`/api/ptz/${id}/absolute`, {
    method: 'POST',
    body: JSON.stringify({ pan, tilt, zoom }),
  }),
  ptzRelative: (id, pan, tilt, zoom) => apiClient.request(`/api/ptz/${id}/relative`, {
    method: 'POST',
    body: JSON.stringify({ pan, tilt, zoom }),
  }),
  ptzGetPresets: (id) => apiClient.request(`/api/ptz/${id}/presets`),
  ptzSavePreset: (id, name, presetToken = null) => apiClient.request(`/api/ptz/${id}/presets`, {
    method: 'POST',
    body: JSON.stringify({ name, preset_token: presetToken }),
  }),
  ptzGoToPreset: (id, presetToken) => apiClient.request(`/api/ptz/${id}/goto-preset`, {
    method: 'POST',
    body: JSON.stringify({ preset_token: presetToken }),
  }),
  ptzStartPatrol: () => apiClient.unsupported('PTZ patrol control is not exposed by the current backend API'),
  ptzStopPatrol: () => apiClient.unsupported('PTZ patrol control is not exposed by the current backend API'),
  ptzAddPatrolEntry: () => apiClient.unsupported('PTZ patrol entries are managed on the frontend because the backend API has no patrol endpoint'),

  getEvents: (params = {}) => apiClient.request(`/api/events${buildQuery(params)}`),
  getEventStats: (cameraId) => apiClient.request(`/api/events/stats${buildQuery(cameraId ? { camera_id: cameraId } : {})}`),
  getEventAnalytics: () => apiClient.request('/api/events/analytics'),
  getEventTimeline: (cameraId) => apiClient.request(`/api/events/timeline${buildQuery(cameraId ? { camera_id: cameraId } : {})}`),
  deleteEvent: (id) => apiClient.request(`/api/events/${id}`, { method: 'DELETE' }),

  getPersons: () => apiClient.request('/api/faces/persons'),
  addPerson: (data) => apiClient.request('/api/faces/persons', { method: 'POST', body: JSON.stringify(data) }),
  updatePerson: (id, data) => apiClient.request(`/api/faces/persons/${id}`, { method: 'PUT', body: JSON.stringify(data) }),
  deletePerson: (id) => apiClient.request(`/api/faces/persons/${id}`, { method: 'DELETE' }),
  getFaceStats: () => apiClient.request('/api/faces/stats'),
  searchFace: (imageBase64, threshold = 0.6) => apiClient.request('/api/faces/search', {
    method: 'POST',
    body: JSON.stringify({ image: imageBase64, threshold }),
  }),

  getAnprPlates: (params = {}) => apiClient.request(`/api/anpr/plates${buildQuery(params)}`),
  searchAnpr: (query) => apiClient.request(`/api/anpr/search${buildQuery({ q: query })}`),
  deleteAnprPlates: () => apiClient.request('/api/anpr/plates', { method: 'DELETE' }),

  getTrafficCounts: (params = {}) => apiClient.request(`/api/traffic/counts${buildQuery(params)}`),
  getTrafficSummary: (cameraId) => apiClient.request(`/api/analytics/summary/${cameraId}`),
  getTrafficHistory: (cameraId) => apiClient.request(`/api/analytics/traffic/${cameraId}`),
  getAnalyticsHeatmap: (cameraId) => apiClient.request(`/api/analytics/heatmap/${cameraId}`),
  exportAnalyticsCsv: (params = {}) => apiClient.download(`/api/analytics/export${buildQuery(params)}`),

  getSystemStats: () => apiClient.request('/api/system/stats'),
  getHealth: () => apiClient.request('/api/health'),
  getSettings: () => apiClient.request('/api/system/settings'),
  updateSettings: (data) => apiClient.request('/api/system/settings', { method: 'PUT', body: JSON.stringify(data) }),
  getAuditLogs: (params = {}) => apiClient.request(`/api/audit-logs${buildQuery(params)}`),
  getAttendance: (date) => apiClient.request(`/api/attendance${buildQuery({ date })}`),

  getDevices: () => apiClient.request('/api/devices'),
  getDevice: (id) => apiClient.request(`/api/devices/${id}`),
  addDevice: (data) => apiClient.request('/api/devices', { method: 'POST', body: JSON.stringify(data) }),
  updateDevice: (id, data) => apiClient.request(`/api/devices/${id}`, { method: 'PUT', body: JSON.stringify(data) }),
  deleteDevice: (id) => apiClient.request(`/api/devices/${id}`, { method: 'DELETE' }),
  syncDevice: (id) => apiClient.request(`/api/devices/${id}/sync`, { method: 'POST' }),
  rebootDevice: (id) => apiClient.request(`/api/devices/${id}/reboot`, { method: 'POST' }),

  getRecordings: (params = {}) => apiClient.request(`/api/recordings${buildQuery(params)}`),
  getRecordingSegments: (params = {}) => apiClient.request(`/api/recordings/segments${buildQuery(params)}`),

  getRules: () => apiClient.request('/api/rules'),
  addRule: (data) => apiClient.request('/api/rules/create', { method: 'POST', body: JSON.stringify(data) }),
  updateRule: (id, data) => apiClient.request(`/api/rules/${id}`, { method: 'PUT', body: JSON.stringify(data) }),
  deleteRule: (id) => apiClient.request(`/api/rules/${id}`, { method: 'DELETE' }),
  getZones: () => apiClient.request('/api/zones'),
  addZone: (data) => apiClient.request('/api/zones/create', { method: 'POST', body: JSON.stringify(data) }),
  updateZone: (id, data) => apiClient.request(`/api/zones/${id}`, { method: 'PUT', body: JSON.stringify(data) }),
  deleteZone: (id) => apiClient.request(`/api/zones/${id}`, { method: 'DELETE' }),
  getRuleStats: () => apiClient.request('/api/rules/stats'),
  getRuleTriggerLogs: () => apiClient.unsupported('Rule trigger logs endpoint is not exposed by the current backend API'),

  getSites: () => apiClient.request('/api/sites'),
  addSite: (siteData) => apiClient.request('/api/sites', { method: 'POST', body: JSON.stringify(siteData) }),
  deleteSite: (id) => apiClient.request(`/api/sites/${id}`, { method: 'DELETE' }),
  getUserPermissions: (userId) => apiClient.request(`/api/permissions/${userId}`),
  updateUserPermissions: (userId, perms) => apiClient.request(`/api/permissions/${userId}`, { method: 'PUT', body: JSON.stringify(perms) }),

  getVideoWallLayouts: () => apiClient.request('/api/videowall/layouts'),
  createVideoWallLayout: (data) => apiClient.request('/api/videowall/layouts', { method: 'POST', body: JSON.stringify(data) }),
  updateVideoWallLayout: (id, data) => apiClient.request(`/api/videowall/layouts/${id}`, { method: 'PUT', body: JSON.stringify(data) }),
  deleteVideoWallLayout: (id) => apiClient.request(`/api/videowall/layouts/${id}`, { method: 'DELETE' }),

  getReIDGallery: () => apiClient.request('/api/reid/gallery'),
  getReIDTrail: (globalId) => apiClient.request(`/api/reid/trail/${globalId}`),
  searchReID: (imageBase64) => apiClient.request('/api/reid/search', { method: 'POST', body: JSON.stringify({ image: imageBase64 }) }),
  getReIDConfig: () => apiClient.request('/api/reid/config'),
  updateReIDConfig: (config) => apiClient.request('/api/reid/config', { method: 'PUT', body: JSON.stringify(config) }),
  clearReIDGallery: () => apiClient.request('/api/reid/gallery', { method: 'DELETE' }),
  getReIDStatistics: () => apiClient.request('/api/reid/statistics'),
};

export default apiClient;
