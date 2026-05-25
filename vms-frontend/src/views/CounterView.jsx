import React, { useState, useEffect } from 'react';
import { Footprints, Car, TrendingUp, Users, AlertCircle, RefreshCw, BarChart, Download, Flame, Plus, Edit2, Trash2, Move } from 'lucide-react';
import useVmsStore from '../store/useVmsStore';
import apiClient from '../api/apiClient';
import CountingLineEditor from '../components/CountingLineEditor';

const CounterView = () => {
  const counts = useVmsStore((state) => state.counts);
  const lineCrossingData = useVmsStore((state) => state.lineCrossingData);
  const cameras = useVmsStore((state) => state.cameras);
  const [trafficSummaries, setTrafficSummaries] = useState([]);
  const [trafficHistory, setTrafficHistory] = useState([]);
  const [loading, setLoading] = useState(true);
  const [retentionDays, setRetentionDays] = useState(30);
  const [capacityThreshold, setCapacityThreshold] = useState(80);
  const [audibleAlert, setAudibleAlert] = useState(true);
  const [savingSettings, setSavingSettings] = useState(false);
  const [showHeatmap, setShowHeatmap] = useState(false);
  const [heatmapData, setHeatmapData] = useState([]);
  const [exporting, setExporting] = useState(false);
  // Backend pipeline health (CounterBucketAggregator + PeopleCountTracker).
  // Polled separately from fetchData so the badge updates even when the
  // operator hasn't hit "Làm mới". Null until the first reply lands —
  // initial render shows a neutral state instead of falsely claiming
  // "active" (operator-visible BUG-FE-COUNTER-STATIC-BADGE-01 class issue).
  const [counterStatus, setCounterStatus] = useState(null);

  // Counting lines management
  const [lines, setLines]               = useState([]);
  const [linesLoading, setLinesLoading] = useState(false);
  const [editorState, setEditorState]   = useState(null); // { cameraId, line }

  // Memoize the camera id list so a fresh array reference from the store
  // doesn't retrigger the heavy fetch loop unnecessarily.
  const cameraIds = React.useMemo(() => cameras.map((c) => c.id).join(','), [cameras]);

  const fetchData = React.useCallback(async () => {
    setLoading(true);
    try {
      if (cameras.length > 0) {
        // PR-1 (2026-05-24): switched from getTrafficSummary/History (which
        // queried `traffic_counts`, a vehicle/ANPR-only table) to the new
        // /api/counter/* endpoints that read counter_buckets_1m. The
        // aggregator writes that table from LINE_CROSSING_* events, so this
        // is the data source the live AI pipeline actually populates.
        const summaries = await Promise.all(cameras.map((cam) => apiClient.getCounterSummary(cam.id)));
        const validSummaries = summaries.filter((s) => s.success).map((s) => s.data);
        setTrafficSummaries(validSummaries);

        const history = await apiClient.getCounterHistory(cameras[0].id);
        if (history.success) {
          setTrafficHistory(history.data?.points || []);
        }
      }

      const settings = await apiClient.getSettings();
      if (settings.success && settings.data) {
        if (settings.data.analytics_retention_days) {
          setRetentionDays(parseInt(settings.data.analytics_retention_days, 10));
        }
        if (settings.data.counter_capacity_threshold) {
          setCapacityThreshold(parseInt(settings.data.counter_capacity_threshold, 10));
        }
        if (settings.data.counter_audible_alert !== undefined) {
          setAudibleAlert(Boolean(settings.data.counter_audible_alert));
        }
      }
    } catch (err) {
      console.error('CounterView: fetch error', err);
    } finally {
      setLoading(false);
    }
    // cameras dep stays via cameraIds memo to avoid identity loop
  }, [cameraIds]); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    fetchData();
  }, [fetchData]);

  const refreshLines = React.useCallback(async () => {
    setLinesLoading(true);
    try {
      const res = await apiClient.getCountingLines();
      if (res.success) setLines(res.data?.lines || []);
    } catch (err) {
      console.error('CounterView: refreshLines error', err);
    } finally {
      setLinesLoading(false);
    }
  }, []);

  useEffect(() => { refreshLines(); }, [refreshLines]);

  // Poll backend pipeline health on mount + every 15s. 15s is intentionally
  // shorter than the aggregator's default 60s sweep interval so a "stopped"
  // state surfaces fast without spamming the API. The endpoint itself is
  // cheap (atomic loads + shared_lock).
  useEffect(() => {
    let cancelled = false;
    const pull = async () => {
      try {
        const res = await apiClient.getCounterStatus();
        if (!cancelled && res.success) setCounterStatus(res.data || null);
      } catch (err) {
        // Network blip — leave the prior state so the badge doesn't flicker.
        console.warn('[CounterView] status poll failed', err);
      }
    };
    pull();
    const id = setInterval(pull, 15000);
    return () => { cancelled = true; clearInterval(id); };
  }, []);

  const handleDeleteLine = async (id) => {
    if (!window.confirm('Xóa vạch đếm này? Không thể hoàn tác.')) return;
    try {
      const res = await apiClient.deleteCountingLine(id);
      if (res.success) {
        await refreshLines();
      } else {
        alert(res.error || 'Xóa thất bại');
      }
    } catch (e) {
      alert(e.message || 'Lỗi mạng');
    }
  };

  const handleThresholdChange = async (val) => {
    const prev = capacityThreshold;
    setCapacityThreshold(val);
    try {
      await apiClient.updateSettings({ counter_capacity_threshold: val });
    } catch (err) {
      console.error('[CounterView] persist capacity threshold failed:', err);
      // Operator slid the threshold — they expect it to stick. Revert
      // optimistic UI + tell them so they don't trust the displayed value.
      setCapacityThreshold(prev);
      window.alert(`Lưu ngưỡng sức chứa thất bại: ${err?.message || err}`);
    }
  };

  const handleAudibleToggle = async () => {
    const prev = audibleAlert;
    const next = !audibleAlert;
    setAudibleAlert(next);
    try {
      await apiClient.updateSettings({ counter_audible_alert: next });
    } catch (err) {
      console.error('[CounterView] persist audible alert failed:', err);
      // Toggle visible UI state — operator must know it didn't persist.
      setAudibleAlert(prev);
      window.alert(`Lưu cảnh báo âm thanh thất bại: ${err?.message || err}`);
    }
  };

  // Build hourly buckets (07h..22h, 16 cells) from server history if available
  const hourlyTraffic = React.useMemo(() => {
    const buckets = new Array(16).fill(0);
    (trafficHistory || []).forEach((h) => {
      const hour = h.hour ?? new Date((h.timestamp || 0) * 1000).getHours();
      const value = h.count ?? h.total ?? ((h.in || 0) + (h.out || 0));
      const idx = hour - 7;
      if (idx >= 0 && idx < buckets.length) buckets[idx] += value;
    });
    return buckets;
  }, [trafficHistory]);
  const maxHourly = Math.max(1, ...hourlyTraffic);

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
            {(() => {
              // Reflect real backend state instead of a static "Active" badge.
              // States:
              //   loading   — first reply hasn't landed yet
              //   ok        — aggregator running AND at least one line loaded
              //   no-lines  — running but no counting_lines configured anywhere
              //   stopped   — aggregator not running (backend down or crashed)
              const agg = counterStatus?.aggregator;
              const trk = counterStatus?.tracker;
              let label = 'Đang tải trạng thái...';
              let color = 'var(--text-dim)';
              let title = '';
              if (counterStatus) {
                if (!agg?.running) {
                  label = 'Aggregator dừng';
                  color = 'var(--danger)';
                  title = 'CounterBucketAggregator chưa start — dashboard sẽ trống.';
                } else if ((trk?.total_lines || 0) === 0) {
                  label = 'Chưa cấu hình vạch đếm';
                  color = 'var(--warn)';
                  title = 'Aggregator đang chạy nhưng counting_lines rỗng.';
                } else {
                  label = `Aggregator hoạt động · ${trk.total_lines} vạch`;
                  color = 'var(--accent3)';
                  title = `Sweeps: ${agg.total_sweeps} · Upserts: ${agg.total_upserted} · Last sweep: ${agg.last_sweep_ms} ms`;
                }
              }
              return (
                <div className="ai-badge" title={title} style={{ color }}>
                  <div className="ai-dot-spin" style={{ background: color }}></div>
                  {label}
                </div>
              );
            })()}
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
              <button className="config-btn" style={{ fontSize: 9 }} onClick={fetchData} disabled={loading}>
                <RefreshCw size={10} /> {loading ? 'Đang tải...' : 'Làm mới'}
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
                      <td>{(typeof s.peak_hour === 'number' && s.peak_hour >= 0) ? `${s.peak_hour}h` : '-'} ({s.peak_count || 0})</td>
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
                <div className="ai-row-label">Ngưỡng tối đa (người)</div>
                <input
                  className="config-input"
                  type="number"
                  min={1}
                  value={capacityThreshold}
                  onChange={(e) => handleThresholdChange(parseInt(e.target.value, 10) || 0)}
                  style={{ width: 60 }}
                />
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
                <div
                  className={`toggle ${audibleAlert ? 'on' : ''}`}
                  style={{ width: 30, height: 15, cursor: 'pointer' }}
                  onClick={handleAudibleToggle}
                >
                  <div className="toggle-dot" />
                </div>
              </div>
              <div className="ai-row">
                <div className="ai-row-label">Reset hằng ngày</div>
                <div className="config-select">00:00</div>
              </div>

              {counts.people > capacityThreshold && (
                <div style={{ marginTop: 10, padding: 10, background: 'rgba(255,48,96,0.05)', borderRadius: 4, border: '1px solid rgba(255,48,96,0.1)' }}>
                  <div style={{ display: 'flex', alignItems: 'center', gap: 8, color: 'var(--danger)', fontSize: 11, fontWeight: 700 }}>
                    <AlertCircle size={14} /> CẢNH BÁO CAPACITY
                  </div>
                  <div style={{ fontSize: 9, color: 'var(--text-secondary)', marginTop: 4 }}>
                    Đã phát hiện {counts.people} người (ngưỡng {capacityThreshold}). Cần điều phối luồng người.
                  </div>
                </div>
              )}
            </div>
          </div>
        </div>

        {/* Counting Lines management */}
        <div className="analytics-card span3 glass" style={{ marginTop: 10 }}>
          <div className="analytics-card-title" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
            <span><Move size={12} style={{ marginRight: 6 }} /> Quản lý vạch đếm (Counting Lines)</span>
            <div style={{ display: 'flex', gap: 8 }}>
              <button className="config-btn" onClick={refreshLines} disabled={linesLoading} style={{ fontSize: 10 }}>
                <RefreshCw size={10} /> {linesLoading ? '...' : 'Làm mới'}
              </button>
            </div>
          </div>

          {cameras.length === 0 ? (
            <div style={{ textAlign: 'center', padding: 24, color: 'var(--text-dim)' }}>
              Chưa có camera nào trong hệ thống.
            </div>
          ) : (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 6, marginTop: 10 }}>
              {cameras.map((cam) => {
                const camLines = lines.filter((ln) => ln.camera_id === cam.id);
                return (
                  <div key={cam.id} style={{
                    border: '1px solid var(--border)', borderRadius: 6,
                    padding: 10, background: 'rgba(0,0,0,0.25)',
                  }}>
                    <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 6 }}>
                      <div style={{ fontSize: 12, fontWeight: 700 }}>
                        {cam.name || `CAM-${cam.id}`} <span style={{ color: 'var(--text-dim)', fontWeight: 400 }}>· {camLines.length} vạch</span>
                      </div>
                      <button className="config-btn"
                              onClick={() => setEditorState({ cameraId: cam.id, line: null })}
                              style={{ padding: '3px 8px', fontSize: 10 }}>
                        <Plus size={11} /> Thêm vạch
                      </button>
                    </div>
                    {camLines.length === 0 ? (
                      <div style={{ fontSize: 10, color: 'var(--text-dim)', padding: '4px 0' }}>
                        Chưa có vạch đếm — bấm "Thêm vạch" để vẽ trên snapshot.
                      </div>
                    ) : (
                      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11 }}>
                        <thead>
                          <tr style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-dim)' }}>
                            <th style={{ textAlign: 'left',  padding: 4 }}>Tên</th>
                            <th style={{ textAlign: 'left',  padding: 4 }}>A→B / B→A</th>
                            <th style={{ textAlign: 'left',  padding: 4 }}>Class</th>
                            <th style={{ textAlign: 'center', padding: 4 }}>Bật</th>
                            <th style={{ textAlign: 'right',  padding: 4 }}>Hành động</th>
                          </tr>
                        </thead>
                        <tbody>
                          {camLines.map((ln) => (
                            <tr key={ln.id} style={{ borderBottom: '1px solid rgba(255,255,255,0.04)' }}>
                              <td style={{ padding: 4 }}>{ln.name || `Line #${ln.id}`}</td>
                              <td style={{ padding: 4, fontFamily: "'Share Tech Mono'", color: 'var(--accent3)' }}>
                                {ln.direction_a_label || 'in'} / {ln.direction_b_label || 'out'}
                              </td>
                              <td style={{ padding: 4, color: 'var(--text-dim)' }}>
                                {(() => { try { return (JSON.parse(ln.object_classes_json || '["person"]') || []).join(', '); } catch { return 'person'; } })()}
                              </td>
                              <td style={{ padding: 4, textAlign: 'center' }}>
                                {ln.enabled ? '✓' : <span style={{ color: 'var(--text-dim)' }}>—</span>}
                              </td>
                              <td style={{ padding: 4, textAlign: 'right' }}>
                                <button className="config-btn"
                                        onClick={() => setEditorState({ cameraId: cam.id, line: ln })}
                                        style={{ padding: '2px 6px', marginRight: 4 }}>
                                  <Edit2 size={10} />
                                </button>
                                <button className="config-btn danger"
                                        onClick={() => handleDeleteLine(ln.id)}
                                        style={{ padding: '2px 6px' }}>
                                  <Trash2 size={10} />
                                </button>
                              </td>
                            </tr>
                          ))}
                        </tbody>
                      </table>
                    )}
                  </div>
                );
              })}
            </div>
          )}
        </div>

        {editorState && (
          <CountingLineEditor
            cameraId={editorState.cameraId}
            cameraName={cameras.find((c) => c.id === editorState.cameraId)?.name}
            initialLine={editorState.line}
            onSave={async () => { await refreshLines(); setEditorState(null); }}
            onClose={() => setEditorState(null)}
          />
        )}

        {/* Capacity Tracking Chart */}
        <div className="analytics-card span3 glass" style={{ marginTop: 10 }}>
          <div className="analytics-card-title">Biểu đồ lưu lượng trong ngày (07h–22h)</div>
          <div className="bar-chart" style={{ height: 120 }}>
            {hourlyTraffic.map((value, i) => {
              const heightPct = (value / maxHourly) * 100;
              const ratio = value / Math.max(1, capacityThreshold);
              const color = ratio > 1 ? 'var(--danger)' : ratio > 0.7 ? 'var(--warn)' : 'var(--accent3)';
              return (
                <div key={i} className="bar-col" title={`${i + 7}h: ${value}`}>
                  <div className="bar-bar" style={{ height: `${Math.max(2, heightPct)}%`, background: color }} />
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
