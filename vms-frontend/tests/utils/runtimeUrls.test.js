import { describe, it, expect, beforeEach, vi } from 'vitest';

const importFresh = async () => {
  vi.resetModules();
  return import('../../src/utils/runtimeUrls.js');
};

describe('runtimeUrls', () => {
  beforeEach(() => {
    vi.unstubAllEnvs();
    vi.unstubAllGlobals();
  });

  it('API_BASE_URL is empty by default (Vite-proxy / same-origin shape)', async () => {
    vi.stubEnv('VITE_API_URL', '');
    const { API_BASE_URL } = await importFresh();
    expect(API_BASE_URL).toBe('');
  });

  it('API_BASE_URL strips trailing slash', async () => {
    vi.stubEnv('VITE_API_URL', 'http://backend.local:8000/');
    const { API_BASE_URL } = await importFresh();
    expect(API_BASE_URL).toBe('http://backend.local:8000');
  });

  it('WS_BASE_URL derives ws:// from http:// API base + appends /ws', async () => {
    vi.stubEnv('VITE_API_URL', 'http://backend.local:8000');
    vi.stubEnv('VITE_WS_URL', '');
    const { WS_BASE_URL } = await importFresh();
    expect(WS_BASE_URL).toBe('ws://backend.local:8000/ws');
  });

  it('WS_BASE_URL derives wss:// from https:// API base', async () => {
    vi.stubEnv('VITE_API_URL', 'https://prod.example.com');
    vi.stubEnv('VITE_WS_URL', '');
    const { WS_BASE_URL } = await importFresh();
    expect(WS_BASE_URL).toBe('wss://prod.example.com/ws');
  });

  it('WS_BASE_URL: explicit VITE_WS_URL wins over derivation', async () => {
    vi.stubEnv('VITE_API_URL', 'http://api.local:8000');
    vi.stubEnv('VITE_WS_URL', 'wss://ws.example.com');
    const { WS_BASE_URL } = await importFresh();
    expect(WS_BASE_URL).toBe('wss://ws.example.com/ws');
  });

  it('WS_BASE_URL: dev fallback when no env vars set', async () => {
    vi.stubEnv('VITE_API_URL', '');
    vi.stubEnv('VITE_WS_URL', '');
    vi.stubGlobal('window', {
      location: { protocol: 'http:', hostname: 'localhost' },
    });
    const { WS_BASE_URL } = await importFresh();
    expect(WS_BASE_URL).toBe('ws://localhost:8083/ws');
  });
});
