import React, { useState } from 'react';
import Topbar from './components/Topbar';
import SidebarLeft from './components/SidebarLeft';
import LiveView from './views/LiveView';
import FaceIdView from './views/FaceIdView';
import FloorMapView from './views/FloorMapView';
import AnalyticsView from './views/AnalyticsView';
import SettingsView from './views/SettingsView';
import PlaybackView from './views/PlaybackView';
import PpeMonitorView from './views/PpeMonitorView';
import CounterView from './views/CounterView';
import AlarmsView from './views/AlarmsView';
import AttendanceView from './views/AttendanceView';
import SiteManager from './views/SiteManager';
import VideoWallView from './views/VideoWallView';
import ReIDView from './views/ReIDView';
import LoginView from './views/LoginView';
import CameraConfigModal from './components/CameraConfigModal';
import SidebarRight from './components/SidebarRight';
import useWebSocket from './hooks/useWebSocket';
import apiClient from './api/apiClient';
import useVmsStore from './store/useVmsStore';
import './index.css';

function App() {
  const [activeTab, setActiveTab] = useState('live');
  const [isModalOpen, setIsModalOpen] = useState(false);
  const [modalData, setModalData] = useState(null);
  const [sidebarsVisible, setSidebarsVisible] = useState(true);
  const [isAuthenticated, setIsAuthenticated] = useState(() => {
    const status = localStorage.getItem('vms_auth_status');
    return status === 'true';
  });

  // Initialize WebSocket - it will check for token internally
  useWebSocket();
  
  const setAlarms = useVmsStore(s => s.setAlarms);
  const setFaces = useVmsStore(s => s.setFaces);
  const setCounts = useVmsStore(s => s.setCounts);
  const setAttendance = useVmsStore(s => s.setAttendance);
  const setCameras = useVmsStore(s => s.setCameras);
  const setSystemStats = useVmsStore(s => s.setSystemStats);

  // Handle logout event
  React.useEffect(() => {
    const handleLogout = () => {
      setIsAuthenticated(false);
    };
    window.addEventListener('vms_logout', handleLogout);
    return () => window.removeEventListener('vms_logout', handleLogout);
  }, []);

  // Initial Data Fetch - only when authenticated
  React.useEffect(() => {
    if (!isAuthenticated) return;

    const loadInitialData = async () => {
      try {
        const [eventsRes, personsRes, systemStatsRes, camerasRes] = await Promise.all([
          apiClient.getEvents({ limit: 50 }),
          apiClient.getPersons(),
          apiClient.getSystemStats(),
          apiClient.getCameras()
        ]);

        const events = eventsRes.success ? (eventsRes.data?.events || []) : [];
        const persons = personsRes.success ? (personsRes.data?.persons || []) : [];
        const systemStats = systemStatsRes.success ? systemStatsRes.data : null;
        const cameras = camerasRes.success ? (camerasRes.data?.cameras || []) : [];

        if (Array.isArray(events)) {
          const alarms = events.map((evt) => ({
            id: evt.id,
            time: evt.timestamp ? new Date(evt.timestamp).toLocaleTimeString('vi-VN') : '',
            type: evt.label || evt.event_type || 'AI',
            desc: evt.message || evt.description || '',
            camera: evt.camera_name || `CAM-${evt.camera_id || '?'}`,
            severity: evt.severity || 'low',
            status: evt.status || 'new',
            img: evt.snapshot_url ? evt.snapshot_url : null
          }));
          setAlarms(alarms);
        }

        if (Array.isArray(persons)) {
          const faces = persons.map((p) => ({
            id: p.id,
            name: p.name,
            type: p.description || 'Nhân viên',
            confidence: p.has_embedding ? 95 : 0,
            time: p.updated_at ? new Date(p.updated_at * 1000).toLocaleTimeString('vi-VN') : '',
            avatar: p.face_image_path ? p.face_image_path : null
          }));
          setFaces(faces);
        }

        if (systemStats) {
          setSystemStats(systemStats);
          setCounts({
            alerts: Array.isArray(events) ? events.length : 0,
          });
        }

        if (Array.isArray(cameras)) {
          const activeCameras = cameras.filter(cam => cam.is_active === 1 || cam.is_active === true || cam.status === 'active');
          setCameras(activeCameras.length > 0 ? activeCameras : cameras);
        }

      } catch (err) {
      }
    };

    loadInitialData();

    // Poll system stats every 10 seconds
    const statsInterval = setInterval(async () => {
      const stats = await apiClient.getSystemStats();
      if (stats.success) setSystemStats(stats.data);
    }, 10000);

    return () => clearInterval(statsInterval);
  }, [setAlarms, setFaces, setCounts, setAttendance, setCameras, setSystemStats, isAuthenticated]);

  // Idle timer for sidebar auto-hide (must be before any conditional return)
  React.useEffect(() => {
    if (!isAuthenticated) return;

    let timeout;
    const resetIdleTimer = () => {
      setSidebarsVisible(true);
      clearTimeout(timeout);
      if (activeTab === 'live') {
        timeout = setTimeout(() => {
          setSidebarsVisible(false);
        }, 60000); // 1 minute
      }
    };

    window.addEventListener('mousemove', resetIdleTimer);
    window.addEventListener('mousedown', resetIdleTimer);
    window.addEventListener('keydown', resetIdleTimer);

    resetIdleTimer();

    return () => {
      window.removeEventListener('mousemove', resetIdleTimer);
      window.removeEventListener('mousedown', resetIdleTimer);
      window.removeEventListener('keydown', resetIdleTimer);
      clearTimeout(timeout);
    };
  }, [activeTab, isAuthenticated]);

  // Modal open event listener (must be before any conditional return)
  React.useEffect(() => {
    const handleOpenModal = (e) => {
      const detail = e.detail || null;
      setModalData(detail);
      setIsModalOpen(true);
      // Only trigger auto-scan when explicitly requested AND not editing a camera
      if (detail && detail.autoScan && !detail.camera) {
        window.__vms_auto_scan = true;
      } else {
        window.__vms_auto_scan = false;
      }
    };
    document.addEventListener('vms:openModal', handleOpenModal);
    return () => document.removeEventListener('vms:openModal', handleOpenModal);
  }, []);

  const handleLoginSuccess = () => {
    setIsAuthenticated(true);
  };

  if (!isAuthenticated) {
    return <LoginView onLoginSuccess={handleLoginSuccess} />;
  }

  // Determine actual visibility based on active tab and idle state
  const isSidebarActive = activeTab === 'live' && sidebarsVisible;

  return (
    <>
      <Topbar activeTab={activeTab} setActiveTab={setActiveTab} />
      <div className="main">
        <SidebarLeft isVisible={isSidebarActive} />
        <div className="center">
          <div className="center-views">
            {activeTab === 'live' && <LiveView />}
            {activeTab === 'faceid' && <FaceIdView />}
            {activeTab === 'playback' && <PlaybackView />}
            {activeTab === 'floormap' && <FloorMapView />}
            {activeTab === 'analytics' && <AnalyticsView />}
            {activeTab === 'ppe' && <PpeMonitorView />}
            {activeTab === 'counter' && <CounterView />}
            {activeTab === 'attendance' && <AttendanceView />}
            {activeTab === 'alarms' && <AlarmsView />}
            {activeTab === 'sites' && <SiteManager />}
            {activeTab === 'videowall' && <VideoWallView />}
            {activeTab === 'reid' && <ReIDView />}
            {activeTab === 'settings' && <SettingsView />}
          </div>
        </div>
        <SidebarRight isVisible={isSidebarActive} />
      </div>

      {isModalOpen && <CameraConfigModal onClose={() => setIsModalOpen(false)} initialData={modalData?.camera} />}
    </>
  );
}

export default App;
