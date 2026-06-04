import { describe, it, expect, beforeEach, vi } from 'vitest';
import apiClient from '../../src/api/apiClient.js';

const mockResponse = ({ status = 200, body = { success: true, data: {}, error: null } } = {}) => ({
  ok: status >= 200 && status < 300,
  status,
  statusText: status === 200 ? 'OK' : 'Error',
  headers: new Headers({ 'content-type': 'application/json' }),
  json: async () => body,
  text: async () => JSON.stringify(body),
  blob: async () => new Blob([JSON.stringify(body)]),
});

beforeEach(() => {
  apiClient.clearToken();
  localStorage.clear();
});

describe('apiClient token lifecycle', () => {
  it('setToken stores in-memory + flips localStorage flag (never persists raw token)', () => {
    apiClient.setToken('jwt-abc');
    expect(apiClient.getToken()).toBe('jwt-abc');
    expect(localStorage.getItem('vms_auth_status')).toBe('true');
    expect(localStorage.getItem('vms_auth_token')).toBeNull();
  });

  it('clearToken wipes both memory and localStorage flag', () => {
    apiClient.setToken('jwt-abc');
    apiClient.clearToken();
    expect(apiClient.getToken()).toBeNull();
    expect(localStorage.getItem('vms_auth_status')).toBeNull();
  });

  it('setToken dispatches vms_auth_changed for WS reconnect', () => {
    const listener = vi.fn();
    window.addEventListener('vms_auth_changed', listener);
    apiClient.setToken('jwt-abc');
    expect(listener).toHaveBeenCalledTimes(1);
    window.removeEventListener('vms_auth_changed', listener);
  });
});

describe('apiClient.request envelope handling', () => {
  it('attaches Authorization header when token is set', async () => {
    apiClient.setToken('jwt-xyz');
    const fetchSpy = vi.spyOn(globalThis, 'fetch').mockResolvedValue(mockResponse());
    await apiClient.request('/api/test');
    const [, options] = fetchSpy.mock.calls[0];
    expect(options.headers.Authorization).toBe('Bearer jwt-xyz');
    expect(options.credentials).toBe('include');
  });

  it('surfaces 403 error visibly (no silent-success — BUG-FE-ANPR-DEL-FAKE class)', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(mockResponse({
      status: 403,
      body: { success: false, data: null, error: { code: 'forbidden', message: 'Forbidden' } },
    }));
    const result = await apiClient.request('/api/anpr/plates', { method: 'DELETE' });
    expect(result.success).toBe(false);
    expect(result.ok).toBe(false);
    expect(result.status).toBe(403);
    expect(result.error).toBe('Forbidden');
  });

  it('401 triggers logout side-effects (token clear + vms_logout event)', async () => {
    apiClient.setToken('jwt-abc');
    const logoutListener = vi.fn();
    window.addEventListener('vms_logout', logoutListener);

    vi.spyOn(globalThis, 'fetch').mockResolvedValue(mockResponse({
      status: 401,
      body: { success: false, data: null, error: 'Unauthorized' },
    }));

    await apiClient.request('/api/test');
    expect(apiClient.getToken()).toBeNull();
    expect(logoutListener).toHaveBeenCalled();
    window.removeEventListener('vms_logout', logoutListener);
  });

  it('network error returns structured failure (does not throw)', async () => {
    vi.spyOn(globalThis, 'fetch').mockRejectedValue(new Error('connection refused'));
    const result = await apiClient.request('/api/test');
    expect(result.success).toBe(false);
    expect(result.status).toBe(0);
    expect(result.error).toBe('connection refused');
  });

  it('extracts contract error.message (not stringified object)', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(mockResponse({
      status: 400,
      body: { success: false, data: null, error: { code: 'bad', message: 'Username required' } },
    }));
    const result = await apiClient.request('/api/users', { method: 'POST', body: '{}' });
    expect(result.error).toBe('Username required');
    expect(result.error).not.toContain('[object Object]');
  });
});
