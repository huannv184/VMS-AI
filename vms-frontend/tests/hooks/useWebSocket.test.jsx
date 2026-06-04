import { describe, it, expect, beforeEach, vi } from 'vitest';
import { renderHook, waitFor, act } from '@testing-library/react';

vi.mock('../../src/utils/streamingConfig.js', () => ({
  resolveWsUrl: vi.fn(async () => 'ws://localhost:8083/ws'),
}));

import useWebSocket from '../../src/hooks/useWebSocket.js';
import useVmsStore from '../../src/store/useVmsStore.js';

class FakeWebSocket {
  static instances = [];
  static OPEN = 1;
  static CONNECTING = 0;
  static CLOSING = 2;
  static CLOSED = 3;

  constructor(url) {
    this.url = url;
    this.readyState = FakeWebSocket.CONNECTING;
    this.sent = [];
    this.onopen = null;
    this.onmessage = null;
    this.onclose = null;
    this.onerror = null;
    FakeWebSocket.instances.push(this);
  }

  send(payload) {
    this.sent.push(payload);
  }

  close() {
    this.readyState = FakeWebSocket.CLOSED;
    if (this.onclose) this.onclose({ code: 1000, reason: 'test' });
  }

  // Test helpers
  _triggerOpen() {
    this.readyState = FakeWebSocket.OPEN;
    if (this.onopen) this.onopen();
  }
  _triggerMessage(data) {
    if (this.onmessage) this.onmessage({ data: JSON.stringify(data) });
  }
}

beforeEach(() => {
  FakeWebSocket.instances = [];
  vi.stubGlobal('WebSocket', FakeWebSocket);
  useVmsStore.setState({
    faces: [], alarms: [], lprData: [],
    counts: { people: 0, vehicles: 0, alerts: 0, ppeViolations: 0 },
    isConnected: false,
  });
  // Mock ticket fetch (success path)
  vi.spyOn(globalThis, 'fetch').mockResolvedValue({
    ok: true,
    json: async () => ({ success: true, data: { ticket: 'tk-123' } }),
  });
});

describe('useWebSocket', () => {
  it('opens WebSocket and sends AUTH frame on connect', async () => {
    renderHook(() => useWebSocket());

    await waitFor(() => expect(FakeWebSocket.instances.length).toBe(1));
    const ws = FakeWebSocket.instances[0];
    expect(ws.url).toBe('ws://localhost:8083/ws');

    act(() => ws._triggerOpen());
    expect(useVmsStore.getState().isConnected).toBe(true);

    const authFrame = JSON.parse(ws.sent[0]);
    expect(authFrame).toEqual({ type: 'AUTH', token: 'tk-123' });
  });

  it('face message → addFace mutates store', async () => {
    renderHook(() => useWebSocket());
    await waitFor(() => expect(FakeWebSocket.instances.length).toBe(1));
    const ws = FakeWebSocket.instances[0];
    act(() => ws._triggerOpen());

    act(() => ws._triggerMessage({
      type: 'EVENT_FACE',
      payload: { id: 'f1', person_name: 'Nam', confidence: 0.92, label: 'KNOWN' },
    }));

    const faces = useVmsStore.getState().faces;
    expect(faces).toHaveLength(1);
    expect(faces[0].name).toBe('Nam');
    expect(faces[0].confidence).toBe(0.92);
  });

  it('LPR event routes to lprData (not alarms)', async () => {
    renderHook(() => useWebSocket());
    await waitFor(() => expect(FakeWebSocket.instances.length).toBe(1));
    const ws = FakeWebSocket.instances[0];
    act(() => ws._triggerOpen());

    act(() => ws._triggerMessage({
      type: 'EVENT_ALARM',
      payload: { event_type: 'LPR', plate: 'XYZ789', camera_id: 1, confidence: 0.8 },
    }));

    expect(useVmsStore.getState().lprData).toHaveLength(1);
    expect(useVmsStore.getState().lprData[0].plate_number).toBe('XYZ789');
    expect(useVmsStore.getState().alarms).toHaveLength(0);
  });

  it('non-LPR EVENT_ALARM routes to alarms', async () => {
    renderHook(() => useWebSocket());
    await waitFor(() => expect(FakeWebSocket.instances.length).toBe(1));
    const ws = FakeWebSocket.instances[0];
    act(() => ws._triggerOpen());

    act(() => ws._triggerMessage({
      type: 'EVENT_ALARM',
      payload: { event_type: 'intrusion', label: 'PERSON', camera_id: 2 },
    }));

    expect(useVmsStore.getState().alarms).toHaveLength(1);
    expect(useVmsStore.getState().alarms[0].type).toBe('PERSON');
  });

  it('malformed JSON does not crash hook (defensive parse)', async () => {
    renderHook(() => useWebSocket());
    await waitFor(() => expect(FakeWebSocket.instances.length).toBe(1));
    const ws = FakeWebSocket.instances[0];
    act(() => ws._triggerOpen());

    expect(() => {
      act(() => {
        if (ws.onmessage) ws.onmessage({ data: 'not json {{{' });
      });
    }).not.toThrow();
  });
});
