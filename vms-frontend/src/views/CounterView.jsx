import React, { useState, useEffect } from 'react';
import { Footprints, Car, TrendingUp, Users, AlertCircle, RefreshCw, BarChart, Download, Flame } from 'lucide-react';
import useVmsStore from '../store/useVmsStore';
import apiClient from '../api/apiClient';

const CounterView = () => {
  const counts = useVmsStore((state) => state.counts);
  const lineCrossingData = useVmsStore((state) => state.lineCrossingData);
  const cameras = useVmsStore((state) => state.cameras);
  const [trafficSummaries, setTrafficSummaries] = useState([]);
  const [loading, setLoading] = useState(true);
  const [retentionDays, setRetentionDays] = useState(30);
  const [savingSettings, setSavingSettings] = useState(false);
  const [showHeatmap, setShowHeatmap] = useState(false);
  const [heatmapData, setHeatmapData] = useState([]);
  const [exporting, setExporting] = useState(false);

  useEffect(() => {
    const fetchData = async () => {
      setLoading(true);
      try {
        // 1. Fetch traffic summaries
        if (cameras.length > 0) {
          const summaries = await Promise.all(
            cameras.map(cam => apiClient.getTrafficSummary(cam.id))
          );
          const validSummaries = summaries.filter((s) => s.success).map((s) => s.data);
          setTrafficSummaries(validSummaries);
        }

        const settings = await apiClient.getSettings();
        if (settings.success && settings.data?.analytics_retention_days) {
          setRetentionDays(parseInt(settings.data.analytics_retention_days, 10));
        }
      } catch (err) {
        console.error('CounterView: fetch error', err);
      } finally {
        setLoading(false);
      }
    };
    fetchData();
  }, [cameras]);

  const handleUpdateRetention = async () => {
    setSavingSettings(true);
    try {
      const res = await apiClient.updateSettings({
        analytics_retention_days: retentionDays
      });
      if (!res.success) {
        throw new Error(res.error || 'Failed to update settings');
      }
      alert('Đã cập nhật ngày lưu trữ dữ liệu!');
    } catch (err) {
      console.error('Failed to update retention', err);
    } finally {
      setSavingSettings(false);
    }
  };

  const handleExport = async (type) => {
    setExporting(true);
    try {
      const res = await apiClient.exportAnalyticsCsv({ type });
      if (!res.success) throw new Error(res.error || 'Failed to export');
      apiClient.saveBlobResponse(res, `traffic_export_${new Date().getTime()}.csv`);
    } catch (err) {
      console.error('Export failed', err);
    } finally {
      setTimeout(() => setExporting(false), 2000);
    }
  };

  const toggleHeatmap = async (camId) => {
    if (showHeatmap) {
      setShowHeatmap(false);
      return;
    }
    
    try {
      const res = await apiClient.getAnalyticsHeatmap(camId);
      if (res.success && res.data?.points) {
        setHeatmapData(res.data.points);
        setShowHeatmap(true);
      }
    } catch (err) {
      console.error('Heatmap fetch failed', err);
    }
  };

  // Aggregate traffic data
  const totalIn = trafficSummaries.reduce((sum, s) => sum + (s.total_in || 0), 0);
  const totalOut = trafficSummaries.reduce((sum, s) => sum + (s.total_out || 0), 0);
  const totalToday = trafficSummaries.reduce((sum, s) => sum + (s.total_today || 0), 0);

  return (
    <div className="analytics-view">
      <div className="analytics-inner">
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 15 }}>
          <div style={{ fontFamily: 'Rajdhani', fontWeight: 700, fontSize: 18, letterSpacing: 2, textTransform: 'uppercase', color: 'var(--accent)' }}>
            Quản lý Đếm Người & Phương tiện
          </div>
          <div style={{ display: 'flex', gap: 10 }}>
            <button 
              className="pb-btn" 
              onClick={() => handleExport('traffic')}
              disabled={exporting}
              style={{ background: 'rgba(255,255,255,0.05)', fontSize: 11, padding: '5px 12px' }}
            >
              <Download size={12} style={{ marginRight: 6 }} /> 
              {exporting ? 'ĐANG TẢI...' : 'XUẤT BÁO CÁO (CSV)'}
            </button>
            <div className="ai-badge">
              <div className="ai-dot-spin"></div>
              Multi-Object Tracker Active
            </div>
          </div>
        </div>

        {/* Live Status Cards */}
        <div className="analytics-grid">
          <div className="analytics-card glass" style={{ borderLeft: '3px solid var(--accent3)' }}>
            <div className="analytics-card-title">Người phát hiện <Users size={12} color="var(--accent3)" /></div>
            <div className="big-number green">{counts.people}</div>
            <div className="big-sub">Tổng vào hôm nay: {totalIn}</div>
          </div>
          <div className="analytics-card glass" style={{ borderLeft: '3px solid var(--accent)' }}>
            <div className="analytics-card-title">Xe phát hiện <Car size={12} color="var(--accent)" /></div>
            <div className="big-number cyan">{counts.vehicles}</div>
            <div className="big-sub">Tổng hôm nay: {totalToday}</div>
          </div>
          <div className="analytics-card glass" style={{ borderLeft: '3px solid var(--purple)' }}>
            <div className="analytics-card-title">Lưu lượng tổng <Footprints size={12} color="var(--purple)" /></div>
            <div className="big-number red">{totalIn + totalOut}</div>
            <div className="big-sub">Vào: {totalIn} · Ra: {totalOut}</div>
          </div>
        </div>

        {/* Line Crossing Details */}
        <div className="analytics-grid">
          <div className="analytics-card span2 glass">
            <div className="analytics-card-title">
              Thống kê đường đếm (Line Crossing)
              <button className="config-btn" style={{ fontSize: 9 }}>
                <RefreshCw size={10} /> Làm mới
              </button>
            </div>
            <table className="lpr-table">
              <thead>
                <tr>
                  <th>Vị trí (Camera)</th>
                  <th>Tổng hôm nay</th>
                  <th>Vào (In)</th>
                  <th>Ra (Out)</th>
                  <th>Giờ cao điểm</th>
                </tr>
              </thead>
              <tbody>
                {trafficSummaries.length > 0 ? trafficSummaries.map((s, idx) => {
                  const cam = cameras.find(c => c.id === s.camera_id);
                  return (
                    <tr key={idx}>
                      <td style={{ fontWeight: 700 }}>{cam?.name || `Camera ${s.camera_id}`}</td>
                      <td style={{ color: 'var(--accent)' }}>{s.total_today || 0}</td>
                      <td style={{ color: 'var(--accent3)' }}>{s.total_in || 0}</td>
                      <td style={{ color: 'var(--danger)' }}>{s.total_out || 0}</td>
                      <td>{s.peak_hour || '-'}h ({s.peak_count || 0})</td>
                      <td style={{ textAlign: 'center' }}>
                         <button 
                           className="config-btn" 
                           onClick={() => toggleHeatmap(s.camera_id)}
                           style={{ color: showHeatmap ? 'var(--warn)' : 'var(--accent)', border: 'none', background: 'transparent' }}
                         >
                           <Flame size={12} />
                         </button>
                      </td>
                    </tr>
                  );
                }) : (
                  <tr>
                    <td colSpan="5" style={{ textAlign: 'center', padding: 20, color: 'var(--text-dim)' }}>
                      {loading ? 'Đang tải...' : 'Chưa có dữ liệu traffic'}
                    </td>
                  </tr>
                )}
                {/* Also show WebSocket line crossing data */}
                {Object.entries(lineCrossingData).map(([gate, data]) => (
                  <tr key={gate}>
                    <td style={{ fontWeight: 700 }}>{gate}</td>
                    <td style={{ color: 'var(--accent)' }}>{(data.in || 0) + (data.out || 0)}</td>
                    <td style={{ color: 'var(--accent3)' }}>{data.in || 0}</td>
                    <td style={{ color: 'var(--danger)' }}>{data.out || 0}</td>
                    <td>+{data.net || 0}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

          <div className="analytics-card glass">
            <div className="analytics-card-title">Cấu hình Cảnh báo</div>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
              <div className="ai-row">
                <div className="ai-row-label">Ngưỡng tối đa (%)</div>
                <input className="config-input" defaultValue="80" style={{ width: 40 }} />
              </div>
              <div className="ai-row">
                <div className="ai-row-label">Ngày lưu trữ (Retention)</div>
                <div style={{ display: 'flex', gap: 5 }}>
                  <input 
                    className="config-input" 
                    type="number"
                    value={retentionDays} 
                    onChange={(e) => setRetentionDays(e.target.value)}
                    style={{ width: 50 }} 
                  />
                  <button 
                    className="config-btn" 
                    onClick={handleUpdateRetention}
                    disabled={savingSettings}
                    style={{ padding: '0 8px', height: 24 }}
                  >
                    {savingSettings ? '...' : 'Lưu'}
                  </button>
                </div>
              </div>
              <div className="ai-row">
                <div className="ai-row-label">Âm báo khu vực</div>
                <div className="toggle on" style={{ width: 30, height: 15 }}><div className="toggle-dot" /></div>
              </div>
              <div className="ai-row">
                <div className="ai-row-label">Reset hằng ngày</div>
                <div className="config-select">00:00</div>
              </div>
              
              {counts.people > 80 && (
                <div style={{ marginTop: 10, padding: 10, background: 'rgba(255,48,96,0.05)', borderRadius: 4, border: '1px solid rgba(255,48,96,0.1)' }}>
                  <div style={{ display: 'flex', alignItems: 'center', gap: 8, color: 'var(--danger)', fontSize: 11, fontWeight: 700 }}>
                    <AlertCircle size={14} /> CẢNH BÁO CAPACITY
                  </div>
                  <div style={{ fontSize: 9, color: 'var(--text-secondary)', marginTop: 4 }}>
                    Số người phát hiện đang ở mức cao. Cần điều phối luồng người.
                  </div>
                </div>
              )}
            </div>
          </div>
        </div>

        {/* Capacity Tracking Chart */}
        <div className="analytics-card span3 glass" style={{ marginTop: 10 }}>
          <div className="analytics-card-title">Biểu đồ lưu lượng trong ngày (In/Out)</div>
          <div className="bar-chart" style={{ height: 120 }}>
            {Array.from({ length: 16 }).map((_, i) => {
              return (
                <div key={i} className="bar-col">
                  <div 
                    className="bar-bar" 
                    style={{ 
                      height: '2px', 
                      background: 'var(--accent3)'
                    }} 
                  />
                  <div className="bar-x">{i + 7}h</div>
                </div>
              );
            })}
          </div>
          <div style={{ display: 'flex', justifyContent: 'center', gap: 20, marginTop: 10 }}>
            <div className="legend-row"><span className="legend-dot" style={{ background: 'var(--accent3)' }}></span> Bình thường</div>
            <div className="legend-row"><span className="legend-dot" style={{ background: 'var(--warn)' }}></span> Cao</div>
            <div className="legend-row"><span className="legend-dot" style={{ background: 'var(--danger)' }}></span> Vượt ngưỡng</div>
          </div>
        </div>
      </div>
    </div>
  );
};

export default CounterView;
