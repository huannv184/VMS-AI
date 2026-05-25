import { API_BASE_URL } from '../utils/runtimeUrls';

const BASE_URL = API_BASE_URL;
// Auth state lives in memory for the current tab. We keep only a lightweight
// "logged in" flag in localStorage; the backend session cookie remains HttpOnly.
const AUTH_CHANGE_EVENT = 'vms_auth_changed';

let isLoggingOut = false;

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

const getToken = () => _inMemoryToken;

const dispatchAuthChanged = (token) => {
  window.dispatchEvent(new CustomEvent(AUTH_CHANGE_EVENT, { detail: { token } }));
};

const setToken = (token, localStorageOnly = false) => {
  if (!localStorageOnly) {
    _inMemoryToken = token || null;
  }

  if (token) {
    // Never persist the bearer token; only persist a UI auth flag.
    localStorage.setItem('vms_auth_status', 'true');
  } else {
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
    const token = _inMemoryToken;
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
      // Always send the session cookie; some flows also attach an in-memory bearer token.
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

  // SEC-001: completes the default-password change gate. Backend will not
  // issue an access token for an admin still on the factory-default password
  // until the user rotates it via this endpoint.
  changePasswordOnLogin: async (tempToken, newPassword) => {
    const result = await apiClient.request('/api/auth/change-password-on-login', {
      method: 'POST',
      body: JSON.stringify({ temp_token: tempToken, new_password: newPassword }),
    });

    const token = result.data?.token;
    if (result.success && token) {
      setToken(token);
    }

    return result;
  },

  getCurrentUser: () => apiClient.request('/api/auth/me'),

  // BUG-C3 FIX: server-side logout bumps token_version so any leaked JWT/cookie
  // for this user is rejected on the next request. Without this call the client
  // only forgot the token locally — a captured token kept full access.
  logout: async () => {
    try {
      await apiClient.request('/api/auth/logout', { method: 'POST', body: '{}' });
    } catch (_) {
      // Network failure must not strand the user logged in client-side.
    }
  },

  getUsers: () => apiClient.request('/api/users'),
  addUser: (data) => apiClient.request('/api/users', { method: 'POST', body: JSON.stringify(data) }),
  updateUser: (id, data) => apiClient.request(`/api/users/${id}`, { method: 'PUT', body: JSON.stringify(data) }),
  deleteUser: (id) => apiClient.request(`/api/users/${id}`, { method: 'DELETE' }),
  // BUG-H2 FIX: backend exposes two distinct endpoints — POST /api/users/change-password
  // for self-service (uses {old_password, new_password}) and POST /api/users/<id>/reset-password
  // for admin reset (uses {new_password}). The previous single PUT /api/users/<id>/password
  // endpoint never existed → every change-password attempt 404'd.
  changePassword: (id, currentPassword, newPassword) => apiClient.request('/api/users/change-password', {
    method: 'POST',
    body: JSON.stringify({ old_password: currentPassword, new_password: newPassword }),
  }),
  resetPassword: (id, newPassword) => apiClient.request(`/api/users/${id}/reset-password`, {
    method: 'POST',
    body: JSON.stringify({ new_password: newPassword }),
  }),
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
  // NOTE: PTZ patrol (preset tour) is orchestrated CLIENT-SIDE by
  // PtzPatrolModal — it walks a list of preset tokens with setTimeout
  // and dwell-time between ptzGoToPreset calls. The three placeholder
  // ptzStart/StopPatrol + ptzAddPatrolEntry stubs that lived here used
  // to throw `unsupported` but had ZERO callers; removed 2026-05-19
  // after audit. If a future contributor wants real server-side patrol
  // (hardware-driven tour with persistent state across page reload),
  // wire matching backend endpoints first.

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
  getPpeCompliance: (windowMinutes = 60) =>
    apiClient.request(`/api/analytics/ppe_compliance${buildQuery({ window_minutes: windowMinutes })}`),

  getSystemStats: () => apiClient.request('/api/system/stats'),
  getHealth: () => apiClient.request('/api/health'),
  getSettings: () => apiClient.request('/api/system/settings'),
  updateSettings: (data) => apiClient.request('/api/system/settings', { method: 'PUT', body: JSON.stringify(data) }),
  getAuditLogs: (params = {}) => apiClient.request(`/api/audit-logs${buildQuery(params)}`),
  // ── Attendance ─────────────────────────────────────────────────────────
  getAttendance: (date) => apiClient.request(`/api/attendance${buildQuery({ date })}`),
  exportAttendanceCsv: (date) => apiClient.download(`/api/attendance/export${buildQuery({ date })}`),
  recordManualPunch: (payload) => apiClient.request('/api/attendance/manual',
    { method: 'POST', body: JSON.stringify(payload) }),
  getAttendanceEmployees: () => apiClient.request('/api/attendance/employees'),
  createAttendanceEmployee: (data) => apiClient.request('/api/attendance/employees',
    { method: 'POST', body: JSON.stringify(data) }),
  updateAttendanceEmployee: (id, data) => apiClient.request(`/api/attendance/employees/${id}`,
    { method: 'PUT', body: JSON.stringify(data) }),
  deleteAttendanceEmployee: (id) => apiClient.request(`/api/attendance/employees/${id}`,
    { method: 'DELETE' }),
  getCameraRoles: () => apiClient.request('/api/attendance/camera-roles'),
  setCameraRole: (camera_id, role) => apiClient.request('/api/attendance/camera-roles',
    { method: 'POST', body: JSON.stringify({ camera_id, role }) }),
  getAttendanceHealth: () => apiClient.request('/api/attendance/health'),
  // ── Shifts (work schedules) ─────────────────────────────────────────────
  getShifts: (includeInactive = false) => apiClient.request(
    `/api/attendance/shifts${includeInactive ? '?include_inactive=1' : ''}`),
  getShift: (id) => apiClient.request(`/api/attendance/shifts/${id}`),
  createShift: (data) => apiClient.request('/api/attendance/shifts',
    { method: 'POST', body: JSON.stringify(data) }),
  updateShift: (id, data) => apiClient.request(`/api/attendance/shifts/${id}`,
    { method: 'PUT', body: JSON.stringify(data) }),
  deleteShift: (id) => apiClient.request(`/api/attendance/shifts/${id}`,
    { method: 'DELETE' }),

  // ── Holidays (skip late/OT computation on holiday dates) ───────────────
  getHolidays: (year) => apiClient.request(
    `/api/attendance/holidays${year ? buildQuery({ year }) : ''}`),
  getHoliday: (id) => apiClient.request(`/api/attendance/holidays/${id}`),
  createHoliday: (data) => apiClient.request('/api/attendance/holidays',
    { method: 'POST', body: JSON.stringify(data) }),
  updateHoliday: (id, data) => apiClient.request(`/api/attendance/holidays/${id}`,
    { method: 'PUT', body: JSON.stringify(data) }),
  deleteHoliday: (id) => apiClient.request(`/api/attendance/holidays/${id}`,
    { method: 'DELETE' }),

  // ── Hardware event subscription health (per camera) ────────────────────
  // Returns { listening, state, brand, total_events, reconnect_count,
  // seconds_since_event, ... }. Surfaced by CameraConfigModal when editing
  // an existing camera so operators can tell "scene quiet" from "wedged".
  getEventSubscriptionStatus: (cameraId) => apiClient.request(
    `/api/cameras/${cameraId}/event_subscription_status`),

  // ── Counter (counting lines) ───────────────────────────────────────────
  getCountingLines: (cameraId) => apiClient.request(
    `/api/counter/lines${cameraId ? buildQuery({ camera_id: cameraId }) : ''}`),
  createCountingLine: (data) => apiClient.request('/api/counter/lines',
    { method: 'POST', body: JSON.stringify(data) }),
  updateCountingLine: (id, data) => apiClient.request(`/api/counter/lines/${id}`,
    { method: 'PUT', body: JSON.stringify(data) }),
  deleteCountingLine: (id) => apiClient.request(`/api/counter/lines/${id}`,
    { method: 'DELETE' }),
  reloadCountingLines: () => apiClient.request('/api/counter/lines/reload',
    { method: 'POST' }),

  // ── Counter analytics (reads counter_buckets_1m, written by the live AI
  //    pipeline via CounterBucketAggregator). Replaces the legacy
  //    getTrafficSummary/getTrafficHistory calls that pointed at the
  //    orthogonal `traffic_counts` table — those queries returned 0 for
  //    line-crossing pipelines because the data was being written to a
  //    different table than CounterView was reading. ────────────────────
  // params: { from, to } unix epoch seconds (optional; defaults to today).
  getCounterSummary: (cameraId, params = {}) => apiClient.request(
    `/api/counter/summary${buildQuery({ camera_id: cameraId, ...params })}`),
  getCounterHistory: (cameraId, params = {}) => apiClient.request(
    `/api/counter/history${buildQuery({ camera_id: cameraId, ...params })}`),
  getCounterStatus: () => apiClient.request('/api/counter/status'),

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
  getRuleTriggerLogs: (params = {}) => apiClient.request(`/api/alerts/triggers${buildQuery(params)}`),

  getSites: () => apiClient.request('/api/sites'),
  addSite: (siteData) => apiClient.request('/api/sites', { method: 'POST', body: JSON.stringify(siteData) }),
  updateSite: (id, data) => apiClient.request(`/api/sites/${id}`, { method: 'PUT', body: JSON.stringify(data) }),
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
