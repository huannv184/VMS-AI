import { create } from 'zustand';

const useVmsStore = create((set) => ({
  // ── Core State (populated by API + WebSocket) ──
  counts: {
    people: 0,
    vehicles: 0,
    alerts: 0,
    ppeViolations: 0
  },
  isConnected: false,
  cameras: [],
  faces: [],
  alarms: [],
  ppeEvents: [],
  vehicleEvents: [],
  attendance: [],

  // ── System Stats (from /api/system/stats) ──
  systemStats: {
    cpu_percent: 0,
    ram_total_gb: 0,
    ram_used_gb: 0,
    ram_percent: 0,
    disk_total_gb: 0,
    disk_used_gb: 0,
    disk_percent: 0,
    gpu_percent: 0,
    gpu_memory_used_mb: 0,
    gpu_memory_total_mb: 0,
    uptime_seconds: 0,
    cpu_model: '',
    ai_accuracy: 0
  },

  // ── Traffic / Line Crossing (from /api/traffic or WebSocket) ──
  lineCrossingData: {},

  // ── Analytics (from /api/events/analytics) ──
  eventAnalytics: [],

  // ── ANPR (from /api/anpr/plates) ──
  lprData: [],

  // ── Audit Logs ──
  auditLogs: [],

  // ── Actions ──
  setCounts: (newCounts) => set((state) => ({ 
    counts: { ...state.counts, ...newCounts } 
  })),
  
  addFace: (face) => set((state) => ({
    faces: [face, ...state.faces].slice(0, 50)
  })),

  addAlarm: (alarm) => set((state) => ({
    alarms: [alarm, ...state.alarms].slice(0, 100)
  })),

  addPpeEvent: (event) => set((state) => ({
    ppeEvents: [event, ...state.ppeEvents].slice(0, 50)
  })),

  addVehicleEvent: (event) => set((state) => ({
    vehicleEvents: [event, ...state.vehicleEvents].slice(0, 50)
  })),

  updateLineCrossing: (gate, data) => set((state) => ({
    lineCrossingData: {
      ...state.lineCrossingData,
      [gate]: { ...state.lineCrossingData[gate], ...data }
    }
  })),

  setAlarms: (alarms) => set({ alarms: Array.isArray(alarms) ? alarms : [] }),
  setFaces: (faces) => set({ faces: Array.isArray(faces) ? faces : [] }),
  setCameras: (cameras) => set({ cameras: Array.isArray(cameras) ? cameras : [] }),
  addCamera: (camera) => set((state) => ({ cameras: [...state.cameras, camera] })),
  updateCamera: (camera) => set((state) => ({
    cameras: state.cameras.map((existing) => existing.id === camera.id ? { ...existing, ...camera } : existing)
  })),
  removeCamera: (id) => set((state) => ({ cameras: state.cameras.filter(c => c.id !== id) })),
  setAttendance: (attendance) => set({ attendance: Array.isArray(attendance) ? attendance : [] }),
  
  setIsConnected: (isConnected) => set({ isConnected }),

  // ── New setters for API data ──
  setSystemStats: (stats) => set({ systemStats: stats }),
  setEventAnalytics: (data) => set({ eventAnalytics: Array.isArray(data) ? data : [] }),
  setLprData: (data) => set({ lprData: Array.isArray(data) ? data : [] }),
  setLineCrossingData: (data) => set({ lineCrossingData: data || {} }),
  setAuditLogs: (logs) => set({ auditLogs: Array.isArray(logs) ? logs : [] }),
  setPpeEvents: (events) => set({ ppeEvents: Array.isArray(events) ? events : [] }),
}));

export default useVmsStore;
