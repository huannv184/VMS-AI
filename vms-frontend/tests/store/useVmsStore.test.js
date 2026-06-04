import { describe, it, expect, beforeEach } from 'vitest';
import useVmsStore from '../../src/store/useVmsStore.js';

const reset = () => {
  useVmsStore.setState({
    counts: { people: 0, vehicles: 0, alerts: 0, ppeViolations: 0 },
    cameras: [],
    alarms: [],
    lprData: [],
    lineCrossingData: {},
  });
};

describe('useVmsStore', () => {
  beforeEach(reset);

  it('setCameras only accepts arrays (silently coerces non-array to [])', () => {
    useVmsStore.getState().setCameras([{ id: 1 }, { id: 2 }]);
    expect(useVmsStore.getState().cameras).toHaveLength(2);

    useVmsStore.getState().setCameras(null);
    expect(useVmsStore.getState().cameras).toEqual([]);

    useVmsStore.getState().setCameras('not-an-array');
    expect(useVmsStore.getState().cameras).toEqual([]);
  });

  it('addAlarm prepends and caps at 100', () => {
    const { addAlarm } = useVmsStore.getState();
    for (let i = 0; i < 150; i++) addAlarm({ id: i });
    const alarms = useVmsStore.getState().alarms;
    expect(alarms).toHaveLength(100);
    expect(alarms[0].id).toBe(149);
    expect(alarms[99].id).toBe(50);
  });

  it('addLpr dedupes consecutive same plate/camera within 10s', () => {
    const { addLpr } = useVmsStore.getState();
    addLpr({ plate_number: 'ABC123', camera_id: 1, detected_at: 1000 });
    addLpr({ plate_number: 'ABC123', camera_id: 1, detected_at: 1005 });
    expect(useVmsStore.getState().lprData).toHaveLength(1);
  });

  it('addLpr keeps record when plate differs OR camera differs OR >10s apart', () => {
    const { addLpr } = useVmsStore.getState();
    addLpr({ plate_number: 'ABC123', camera_id: 1, detected_at: 1000 });
    addLpr({ plate_number: 'XYZ999', camera_id: 1, detected_at: 1001 });
    addLpr({ plate_number: 'ABC123', camera_id: 2, detected_at: 1002 });
    addLpr({ plate_number: 'ABC123', camera_id: 1, detected_at: 1020 });
    expect(useVmsStore.getState().lprData).toHaveLength(4);
  });

  it('addLpr skips entries without plate_number (defensive)', () => {
    const { addLpr } = useVmsStore.getState();
    addLpr(null);
    addLpr({ plate_number: '', camera_id: 1 });
    addLpr({ camera_id: 1 });
    expect(useVmsStore.getState().lprData).toHaveLength(0);
  });

  it('updateLineCrossing merges nested gate data without clobbering siblings', () => {
    const { updateLineCrossing } = useVmsStore.getState();
    updateLineCrossing('gateA', { in: 5, out: 3 });
    updateLineCrossing('gateB', { in: 1, out: 0 });
    updateLineCrossing('gateA', { in: 7 });
    const data = useVmsStore.getState().lineCrossingData;
    expect(data.gateA).toEqual({ in: 7, out: 3 });
    expect(data.gateB).toEqual({ in: 1, out: 0 });
  });

  it('setCounts merges instead of replacing (partial updates safe)', () => {
    const { setCounts } = useVmsStore.getState();
    setCounts({ people: 10 });
    setCounts({ vehicles: 5 });
    expect(useVmsStore.getState().counts).toEqual({
      people: 10, vehicles: 5, alerts: 0, ppeViolations: 0,
    });
  });
});
