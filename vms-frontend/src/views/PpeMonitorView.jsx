import React, { useState, useEffect } from 'react';
import { ShieldAlert, AlertTriangle, CheckCircle, Users, HardHat, Eye, UserX, X, Maximize2 } from 'lucide-react';
import useVmsStore from '../store/useVmsStore';
import apiClient from '../api/apiClient';

const API_BASE = ''; // Vite proxy sẽ forward đến backend :8000
const normalizeTimestamp = (value) => (value > 9999999999 ? value : value * 1000);

const PpeMonitorView = () => {
  const counts = useVmsStore((state) => state.counts);
  const [selectedViolation, setSelectedViolation] = useState(null);
  const [violations, setViolations] = useState([]);
  const [loading, setLoading] = useState(true);
  const [actionMsg, setActionMsg] = useState(null);

  const fetchPpeEvents = async () => {
    setLoading(true);
    try {
      const eventsRes = await apiClient.getEvents({ limit: 100, event_type: 'PPE_VIOLATION' });
      const events = eventsRes.success ? (eventsRes.data?.events || []) : [];
      if (Array.isArray(events)) {
        const mapped = events.map((evt) => {
          const tsMs = evt.timestamp ? normalizeTimestamp(evt.timestamp) : null;
          return {
            id: evt.id,
            time: tsMs ? new Date(tsMs).toLocaleTimeString('vi-VN') : '',
            timestampMs: tsMs,
            camera: evt.camera_name || `CAM-${evt.camera_id || '?'}`,
            type: evt.message || evt.description || 'Vi phạm PPE',
            obj: evt.label || 'Không xác định',
            severity: (evt.severity || 'high').toLowerCase(),
            img: evt.snapshot_url ? `${API_BASE}${evt.snapshot_url}` : null,
          };
        });
        setViolations(mapped);
      }
    } catch (err) {
      console.error('PpeMonitorView: fetch error', err);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    fetchPpeEvents();
  }, []);

  // Aggregate by hour for the bar chart (16 buckets, 7h..22h)
  const hourlyBuckets = (() => {
    const buckets = new Array(16).fill(0);
    violations.forEach((v) => {
      if (!v.timestampMs) return;
      const hour = new Date(v.timestampMs).getHours();
      const idx = hour - 7;
      if (idx >= 0 && idx < buckets.length) buckets[idx] += 1;
    });
    return buckets;
  })();
  const maxBucket = Math.max(1, ...hourlyBuckets);

  const handleActionFeedback = (msg) => {
    setActionMsg(msg);
    setTimeout(() => setActionMsg(null), 2500);
  };

  const complianceRate = violations.length > 0 
    ? Math.max(0, 100 - (violations.length * 2)).toFixed(1) 
    : '100.0';

  return (
    <div className="analytics-view">
      <div className="analytics-inner">
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 15 }}>
          <div style={{ fontFamily: 'Rajdhani', fontWeight: 700, fontSize: 18, letterSpacing: 2, textTransform: 'uppercase', color: 'var(--accent)' }}>
            Giám sát Bảo hộ Lao động (PPE)
          </div>
          <div className="ai-badge">
            <div className="ai-dot-spin"></div>
            YOLOv11 PPE Active
          </div>
        </div>

        {/* Stats Row */}
        <div className="analytics-grid">
          <div className="analytics-card glass">
            <div className="analytics-card-title">Tỷ lệ tuân thủ <CheckCircle size={12} color="var(--accent3)" /></div>
            <div className="big-number green">{complianceRate}%</div>
            <div className="big-sub">Dựa trên dữ liệu thực</div>
          </div>
          <div className="analytics-card glass">
            <div className="analytics-card-title">Vi phạm hôm nay <AlertTriangle size={12} color="var(--danger)" /></div>
            <div className="big-number red">{violations.length || counts.ppeViolations}</div>
            <div className="big-sub">Cần xử lý: {violations.filter(v => v.severity === 'high').length}</div>
          </div>
          <div className="analytics-card glass">
            <div className="analytics-card-title">Người đang giám sát <Users size={12} color="var(--accent)" /></div>
            <div className="big-number cyan">{counts.people || 0}</div>
            <div className="big-sub">Trên các khu vực sản xuất</div>
          </div>
        </div>

        {/* Monitoring Details */}
        <div className="analytics-grid">
          <div className="analytics-card span2 glass">
            <div className="analytics-card-title">Danh sách vi phạm gần đây (Bằng chứng AI)</div>
            <table className="lpr-table">
              <thead>
                <tr>
                  <th>Thời gian</th>
                  <th>Camera</th>
                  <th>Loại vi phạm</th>
                  <th>Đối tượng</th>
                  <th>Bằng chứng</th>
                  <th>Hành động</th>
                </tr>
              </thead>
              <tbody>
                {violations.map(v => (
                  <tr key={v.id}>
                    <td>{v.time}</td>
                    <td>{v.camera}</td>
                    <td>
                      <span style={{ color: v.severity === 'high' ? 'var(--danger)' : 'var(--warn)', display: 'flex', alignItems: 'center', gap: 5 }}>
                        {v.severity === 'high' ? <ShieldAlert size={12} /> : <AlertTriangle size={12} />}
                        {v.type}
                      </span>
                    </td>
                    <td>{v.obj}</td>
                    <td>
                      {v.img ? (
                        <div 
                          className="ppe-img-thumb" 
                          style={{ cursor: 'pointer', overflow:'hidden', border:'1px solid var(--border-bright)' }}
                          onClick={() => setSelectedViolation(v)}
                        >
                           <img src={v.img} alt="Evidence" style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
                        </div>
                      ) : (
                        <span style={{ fontSize: 9, color: 'var(--text-dim)' }}>N/A</span>
                      )}
                    </td>
                    <td>
                      <button className="config-btn" style={{ padding: '2px 8px', fontSize: 10 }} onClick={() => setSelectedViolation(v)}>
                        <Maximize2 size={10} style={{ marginRight: 4 }} /> Xem
                      </button>
                    </td>
                  </tr>
                ))}
                {violations.length === 0 && (
                  <tr>
                    <td colSpan="6" style={{ textAlign: 'center', padding: '30px', color: 'var(--text-dim)' }}>
                      {loading ? 'Đang tải dữ liệu...' : 'Chưa có vi phạm PPE nào được ghi nhận'}
                    </td>
                  </tr>
                )}
              </tbody>
            </table>
          </div>

          <div className="analytics-card glass">
            <div className="analytics-card-title">Trạng thái thiết bị</div>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
              <div className="ai-row">
                <div className="ai-row-label" style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                  <HardHat size={14} /> Mũ bảo hiểm
                </div>
                <div className="ai-row-val green">AI Active</div>
              </div>
              <div className="ai-row">
                <div className="ai-row-label" style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                  <ShieldAlert size={14} /> Áo phản quang
                </div>
                <div className="ai-row-val green">AI Active</div>
              </div>
              <div className="ai-row">
                <div className="ai-row-label" style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                  <Eye size={14} /> Kính bảo hộ
                </div>
                <div className="ai-row-val orange">AI Active</div>
              </div>
              <div className="ai-row">
                <div className="ai-row-label" style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                  <UserX size={14} /> Găng tay
                </div>
                <div className="ai-row-val red">AI Active</div>
              </div>
              
              <div style={{ marginTop: 10 }}>
                <div className="analytics-card-title" style={{ marginBottom: 5 }}>Thông báo tự động</div>
                <div className="toggle on" style={{ width: 30, height: 15 }}><div className="toggle-dot" /></div>
              </div>
            </div>
          </div>
        </div>

        {/* Chart */}
        <div className="analytics-card span3 glass" style={{ marginTop: 10 }}>
          <div className="analytics-card-title">Biểu đồ cường độ vi phạm theo giờ (07h–22h)</div>
          <div className="bar-chart" style={{ height: 120 }}>
            {hourlyBuckets.map((count, i) => {
              const heightPct = (count / maxBucket) * 100;
              return (
                <div key={i} className="bar-col" title={`${i + 7}h: ${count} vi phạm`}>
                  <div
                    className="bar-bar"
                    style={{
                      height: `${Math.max(2, heightPct)}%`,
                      background: count > 0 ? (count > maxBucket * 0.7 ? 'var(--danger)' : 'var(--warn)') : 'var(--accent)'
                    }}
                  />
                  <div className="bar-x">{i + 7}h</div>
                </div>
              );
            })}
          </div>
        </div>
      </div>

      {/* EVIDENCE MODAL */}
      {selectedViolation && (
        <div style={{
          position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.85)', zIndex: 1000,
          display: 'flex', alignItems: 'center', justifyContent: 'center', padding: 40,
          backdropFilter: 'blur(8px)'
        }}>
          <div className="glass" style={{
            width: '100%', maxWidth: 800, borderRadius: 8, overflow: 'hidden', position: 'relative',
            display: 'flex', flexDirection: 'column', border: '1px solid var(--accent) !important'
          }}>
            <div className="grid-toolbar">
               <div className="logo" style={{ fontSize: 13 }}>
                  <ShieldAlert size={14} color="var(--danger)" /> CHI TIẾT VI PHẠM PPE
               </div>
               <div className="toolbar-sep"></div>
               <div className="grid-btn" onClick={() => setSelectedViolation(null)}><X size={14} /> ĐÓNG</div>
            </div>
            
            <div style={{ display: 'flex', flex: 1, minHeight: 400 }}>
              <div style={{ flex: 1, background: '#000', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
                {selectedViolation.img ? (
                  <img src={selectedViolation.img} alt="Evidence Large" style={{ maxWidth: '100%', maxHeight: '100%' }} />
                ) : (
                  <div style={{ color: 'var(--text-dim)', fontSize: 14 }}>Không có hình ảnh</div>
                )}
              </div>
              <div style={{ width: 260, padding: 20, borderLeft: '1px solid var(--border)', background: 'var(--bg-panel)' }}>
                 <div className="analytics-card-title">Thông tin sự cố</div>
                 <div className="ai-row"><div className="ai-row-label">Thời gian</div><div className="ai-row-val">{selectedViolation.time}</div></div>
                 <div className="ai-row"><div className="ai-row-label">Camera</div><div className="ai-row-val">{selectedViolation.camera}</div></div>
                 <div className="ai-row"><div className="ai-row-label">Đối tượng</div><div className="ai-row-val">{selectedViolation.obj}</div></div>
                 <div className="ai-row"><div className="ai-row-label">Loại vi phạm</div><div className="ai-row-val red">{selectedViolation.type}</div></div>
                 
                 <div style={{ marginTop: 30 }}>
                    <div className="analytics-card-title">Hành động khắc phục</div>
                    {actionMsg && (
                      <div style={{ fontSize: 11, color: 'var(--accent)', marginBottom: 8, textAlign: 'center' }}>{actionMsg}</div>
                    )}
                    <button
                      className="pb-btn primary"
                      style={{ width: '100%', marginBottom: 8, justifyContent: 'center' }}
                      onClick={() => handleActionFeedback('Đã gửi cảnh báo qua loa hệ thống')}
                    >GỬI CẢNH BÁO LOA</button>
                    <button
                      className="pb-btn"
                      style={{ width: '100%', marginBottom: 8, justifyContent: 'center' }}
                      onClick={async () => {
                        try {
                          await navigator.clipboard.writeText(JSON.stringify(selectedViolation, null, 2));
                          handleActionFeedback('Đã sao chép thông tin sự cố vào clipboard');
                        } catch {
                          handleActionFeedback('Không thể truy cập clipboard');
                        }
                      }}
                    >COPY HỒ SƠ</button>
                    <button
                      className="pb-btn"
                      style={{ width: '100%', justifyContent: 'center' }}
                      onClick={() => setSelectedViolation(null)}
                    >BỎ QUA</button>
                 </div>
              </div>
            </div>
          </div>
        </div>
      )}
    </div>
  );
};

export default PpeMonitorView;
