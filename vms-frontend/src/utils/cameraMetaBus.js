/**
 * CameraMetaBus — Lightweight pub/sub for AI metadata.
 * 
 * PURPOSE: Decouple high-frequency AI detection data (15 updates/sec/camera)
 * from React's Zustand store to prevent catastrophic re-renders on LiveView.
 * 
 * CameraFeed subscribes directly via .on(cameraId, cb) and draws detections
 * on its own <canvas> using requestAnimationFrame — zero React involvement.
 * 
 * useWebSocket publishes via .emit(cameraId, detections) when it receives
 * METADATA/camera_metadata messages from the backend /ws endpoint.
 */

const listeners = new Map(); // cameraId → Set<callback>

const cameraMetaBus = {
  /**
   * Subscribe to metadata updates for a specific camera.
   * @param {number|string} cameraId
   * @param {(detections: Array) => void} callback
   * @returns {() => void} unsubscribe function
   */
  on(cameraId, callback) {
    const key = String(cameraId);
    if (!listeners.has(key)) listeners.set(key, new Set());
    listeners.get(key).add(callback);
    return () => {
      const set = listeners.get(key);
      if (set) {
        set.delete(callback);
        if (set.size === 0) listeners.delete(key);
      }
    };
  },

  /**
   * Publish metadata detections for a camera.
   * Runs synchronously — all subscribers are invoked immediately.
   * @param {number|string} cameraId
   * @param {Array} detections
   */
  emit(cameraId, detections) {
    const set = listeners.get(String(cameraId));
    if (set) {
      for (const cb of set) {
        try { cb(detections); } catch { /* subscriber error — isolate */ }
      }
    }
  },

  /** Clear all subscriptions (for testing / hot-reload cleanup) */
  clear() {
    listeners.clear();
  }
};

export default cameraMetaBus;
