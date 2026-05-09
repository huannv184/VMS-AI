import React, { useState, useEffect, useMemo } from 'react';
import { Download, Calendar, Search } from 'lucide-react';
import apiClient from '../api/apiClient';

// eslint-disable-next-line no-unused-vars
const API_BASE = ''; // Vite proxy forward đến :8000

const AnalyticsView = () => {
  const [lprData, setLprData] = useState([]);
  const [lprSearch, setLprSearch] = useState('');
  
  // States to hold the raw backend data
  const [analyticsItems, setAnalyticsItems] = useState([]);
  const [totalAlerts, setTotalAlerts] = useState(0);
  const [loading, setLoading] = useState(true);
  const [exporting, setExporting] = useState(false);

  const handleExportReport = async () => {
    setExporting(true);
    try {
      const res = await apiClient.exportAnalyticsCsv({ type: 'analytics', days: 7 });
      if (res.success) {
        apiClient.saveBlobResponse(res, `analytics_report_${Date.now()}.csv`);
      } else {
        window.alert('Xuất báo cáo thất bại: ' + (res.error || 'Unknown error'));
      }
    } catch (err) {
      window.alert('Lỗi xuất báo cáo: ' + (err.message || err));
    } finally {
      setExporting(false);
    }
  };

  useEffect(() => {
    const fetchData = async () => {
      setLoading(true);
      try {
        const [platesRes, analyticsRes, statsRes] = await Promise.all([
          apiClient.getAnprPlates({ limit: 500 }),
          apiClient.getEventAnalytics(),
          apiClient.getEventStats()
        ]);

        if (platesRes.success) {
          const platesPayload = platesRes.data || {};
          setLprData(platesPayload.plates || (Array.isArray(platesPayload) ? platesPayload : []));
        }

        if (analyticsRes.success && analyticsRes.data?.analytics) {
          setAnalyticsItems(analyticsRes.data.analytics);
        }
        
        if (statsRes.success && statsRes.data?.total_events !== undefined) {
          setTotalAlerts(statsRes.data.total_events);
        }
      } catch (err) {
        console.error('AnalyticsView: fetch error', err);
      } finally {
        setLoading(false);
      }
    };
    fetchData();
  }, []);

  const handleLprSearch = async () => {
    // If the backend has a search API you can call it here.
    // For now we simulate or refetch with no filter if empty
    if (!lprSearch.trim()) {
      const platesRes = await apiClient.getAnprPlates({ limit: 50 });
      if (platesRes.success) {
        const platesPayload = platesRes.data || {};
        setLprData(platesPayload.plates || (Array.isArray(platesPayload) ? platesPayload : []));
      }
      return;
    }
    
    // Simulate searching locally if backend doesn't support plate_number directly via param
    const lowerSearch = lprSearch.toLowerCase();
    const filtered = lprData.filter(p => p.plate_number && p.plate_number.toLowerCase().includes(lowerSearch));
    setLprData(filtered);
  };

  // --------------------------------------------------------
  // Data Processing for Charts
  // --------------------------------------------------------

  // 1. Generate an array of the last 7 days (YYYY-MM-DD)
  const last7Days = useMemo(() => {
    const arr = [];
    for (let i = 6; i >= 0; i--) {
      const d = new Date();
      d.setDate(d.getDate() - i);
      const yyyy = d.getFullYear();
      const mm = String(d.getMonth() + 1).padStart(2, '0');
      const dd = String(d.getDate()).padStart(2, '0');
      arr.push(`${yyyy}-${mm}-${dd}`);
    }
    return arr;
  }, []);

  // 2. Map analytics data to the 7 days array to get Total Counts per Day
  const chartData7Days = useMemo(() => {
    const map = {};
    last7Days.forEach(day => { map[day] = 0; });
    
    analyticsItems.forEach(item => {
      if (map[item.day] !== undefined) {
        map[item.day] += item.count;
      }
    });
    
    // Return an array of counts corresponding to last7Days
    return last7Days.map(day => map[day]);
  }, [analyticsItems, last7Days]);

  const maxDayCount = Math.max(...chartData7Days, 10);

  // 3. Group by Event Type for the Donut Chart
  const typeCounts = useMemo(() => {
    const counts = {};
    let total = 0;
    analyticsItems.forEach(item => {
      counts[item.type] = (counts[item.type] || 0) + item.count;
      total += item.count;
    });
    return { counts, total };
  }, [analyticsItems]);

  // Derived Donut Chart segments
  const donutSegments = useMemo(() => {
    const types = Object.keys(typeCounts.counts).sort((a,b) => typeCounts.counts[b] - typeCounts.counts[a]);
    let segments = [];
    let currentOffset = 0;
    const colors = ['var(--accent)', 'var(--warn)', 'var(--danger)', 'var(--accent3)', 'var(--text-dim)'];
    
    types.forEach((type, index) => {
      const percentage = typeCounts.total > 0 ? (typeCounts.counts[type] / typeCounts.total) * 100 : 0;
      const strokeDashDasharray = `${(percentage / 100) * 150.8} 150.8`;
      const strokeDashoffset = -currentOffset;
      
      segments.push({
        type,
        count: typeCounts.counts[type],
        percentage: percentage.toFixed(1),
        color: colors[index % colors.length],
        strokeDashDasharray,
        strokeDashoffset
      });
      
      currentOffset += (percentage / 100) * 150.8;
    });
    return segments;
  }, [typeCounts]);

  const totalDetections = typeCounts.total;

  return (
    <div className="analytics-view active" style={{padding:'20px', overflowY:'auto'}}>
      <div className="analytics-inner" style={{maxWidth:'1200px', margin:'0 auto'}}>
        
        {/* Header */}
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 20 }}>
          <div style={{ fontFamily: 'Rajdhani', fontWeight: 700, fontSize: 22, letterSpacing: 2, textTransform: 'uppercase', color: 'var(--accent)' }}>
            Hệ Thống Phân Tích & Báo Cáo
          </div>
          <div style={{ display: 'flex', gap: 10 }}>
            <button
              className="pb-btn"
              style={{ padding: '6px 12px', background:'var(--bg)', border:'1px solid var(--border)', opacity: exporting ? 0.5 : 1, cursor: exporting ? 'wait' : 'pointer' }}
              onClick={handleExportReport}
              disabled={exporting}
            >
              <Download size={14} style={{marginRight:6}}/> {exporting ? 'Đang xuất...' : 'Xuất Báo Cáo (CSV)'}
            </button>
            <div className="pb-btn" style={{ background:'var(--accent)', color:'#000', fontWeight:'bold' }}>
              <Calendar size={14} style={{marginRight:6}}/> 7 Ngày qua
            </div>
          </div>
        </div>

        {/* Top Stats */}
        <div className="analytics-grid" style={{marginBottom: 15}}>
          <div className="analytics-card glass">
            <div className="analytics-card-title">Tổng phát hiện <span className="trend-up" style={{color:'var(--accent)', marginLeft:10, fontSize:'10px'}}>↑ TỔNG CỘNG</span></div>
            <div className="big-number cyan">{totalDetections || 0}</div>
            <div className="big-sub">Đối tượng ghi nhận / 7 ngày</div>
          </div>
          <div className="analytics-card glass">
            <div className="analytics-card-title">Lượt xe ra vào (ANPR)</div>
            <div className="big-number green">{lprData.length}</div>
            <div className="big-sub">Đã được bắt biển số</div>
          </div>
          <div className="analytics-card glass">
            <div className="analytics-card-title">Cảnh báo hệ thống (LIVE)</div>
            <div className="big-number red">{totalAlerts}</div>
            <div className="big-sub">Tổng sự kiện trong database</div>
          </div>
        </div>

        {/* Charts Middle Row */}
        <div className="analytics-grid">
          
          {/* Bar Chart 7 Days */}
          <div className="analytics-card span2 glass">
            <div className="analytics-card-title" style={{marginBottom: '20px'}}>Tần suất sự kiện (7 Ngày gần nhất)</div>
            <div className="bar-chart" style={{ height: 180, display:'flex', alignItems:'flex-end', gap:'8%', padding:'10px 0', borderBottom:'1px solid var(--border)' }}>
              {chartData7Days.map((val, i) => {
                const heightPercent = (val / maxDayCount) * 100;
                return (
                  <div key={i} style={{flex: 1, display:'flex', flexDirection:'column', alignItems:'center', justifyContent:'flex-end', height:'100%', position:'relative', group:'hover'}}>
                    <div style={{fontSize:'10px', color:'var(--text-primary)', marginBottom: '5px'}}>{val}</div>
                    <div style={{ 
                      width: '100%', 
                      height: `${Math.max(heightPercent, 1)}%`, 
                      background: i === 6 ? 'var(--accent)' : 'var(--bg-hover)',
                      borderRadius: '3px 3px 0 0',
                      transition: 'height 0.5s ease',
                      border: '1px solid var(--border)'
                    }} />
                    <div style={{position:'absolute', bottom: '-25px', fontSize:'9px', fontFamily:"'Share Tech Mono',monospace", color:'var(--text-secondary)'}}>
                      {last7Days[i].split('-').slice(1).join('/')}
                    </div>
                  </div>
                );
              })}
            </div>
            <div style={{height: '25px'}}></div>
          </div>
          
          {/* Donut Chart */}
          <div className="analytics-card glass">
            <div className="analytics-card-title">Phân loại Dữ liệu</div>
            <div style={{display:'flex', flexDirection:'column', alignItems:'center', marginTop:'15px'}}>
              <div style={{position: 'relative', width:'120px', height:'120px'}}>
                <svg viewBox="0 0 60 60" width="120" height="120" style={{transform: 'rotate(-90deg)'}}>
                  <circle cx="30" cy="30" r="24" fill="none" stroke="var(--bg)" strokeWidth="8" />
                  {donutSegments.map((seg, i) => (
                    <circle 
                      key={i}
                      cx="30" cy="30" r="24" 
                      fill="none" 
                      stroke={seg.color} 
                      strokeWidth="8" 
                      strokeDasharray={seg.strokeDashDasharray} 
                      strokeDashoffset={seg.strokeDashoffset} 
                      style={{transition:'stroke-dasharray 1s ease, stroke-dashoffset 1s ease'}}
                    />
                  ))}
                </svg>
                <div style={{position:'absolute', inset:0, display:'flex', flexDirection:'column', alignItems:'center', justifyContent:'center'}}>
                   <div style={{fontSize:'18px', fontWeight:700, fontFamily:"'Rajdhani',sans-serif", color:'var(--text-primary)'}}>{typeCounts.total}</div>
                   <div style={{fontSize:'9px', color:'var(--text-dim)'}}>SỰ KIỆN</div>
                </div>
              </div>
              
              <div style={{width:'100%', marginTop:'20px', display:'flex', flexDirection:'column', gap:'8px'}}>
                {donutSegments.length > 0 ? donutSegments.map((seg, i) => (
                  <div key={i} style={{display:'flex', justifyContent:'space-between', alignItems:'center', fontSize:'11px'}}>
                    <div style={{display:'flex', alignItems:'center', gap:'8px'}}>
                      <span style={{width:'8px', height:'8px', borderRadius:'50%', background: seg.color}}></span>
                      <span style={{color:'var(--text-secondary)', textTransform:'uppercase'}}>{seg.type}</span>
                    </div>
                    <div style={{fontFamily:"'Share Tech Mono',monospace"}}>
                      <span style={{color:'var(--text-primary)', marginRight:'10px'}}>{seg.count}</span>
                      <span style={{color:seg.color}}>{seg.percentage}%</span>
                    </div>
                  </div>
                )) : (
                  <div style={{textAlign:'center', color:'var(--text-dim)', fontSize:'11px'}}>Chưa có phân loại mẫu</div>
                )}
              </div>
            </div>
          </div>
        </div>

        {/* LPR History Table */}
        <div className="analytics-card span3 glass" style={{ marginTop: 15 }}>
          <div className="analytics-card-title" style={{display:'flex', justifyContent:'space-between'}}>
            <span>Nhật ký Nhận dạng Biển số xe (ANPR Log)</span>
            <div style={{ display: 'flex', gap: 5 }}>
               <input 
                 className="config-input" 
                 placeholder="Tìm biển số..." 
                 style={{ width: 150, padding:'4px 10px', fontSize: '11px', background:'var(--bg)', border:'1px solid var(--border)', color:'var(--text)' }} 
                 value={lprSearch}
                 onChange={(e) => setLprSearch(e.target.value)}
                 onKeyDown={(e) => e.key === 'Enter' && handleLprSearch()}
               />
                <button className="grid-btn" style={{ padding:'0 10px' }} onClick={handleLprSearch}><Search size={12} /></button>
               <button className="grid-btn" style={{ padding:'0 10px', color: 'var(--danger)', border: '1px solid var(--danger)', marginLeft: '10px' }} onClick={async () => {
                 if (window.confirm("Xác nhận XÓA SẠCH dữ liệu nhật ký ANPR rác này?")) {
                   // FE-1 FIX (2026-05-09): backend tightened ANPR DELETE to
                   // SYSTEM_ADMIN. Pre-fix this code didn't check the result
                   // and showed "Đã xóa xong!" even when the server returned
                   // 403 — DB unchanged but local state cleared, refresh
                   // brought everything back. Now we check success and only
                   // clear local state on confirmed delete.
                   const res = await apiClient.deleteAnprPlates();
                   if (res?.success) {
                     setLprData([]);
                     setLprSearch('');
                     window.alert("Đã xóa xong!");
                   } else if (res?.status === 403) {
                     window.alert("Bạn không có quyền xóa nhật ký ANPR (cần quyền admin).");
                   } else if (res?.status === 401) {
                     window.alert("Phiên đăng nhập đã hết hạn.");
                   } else {
                     window.alert("Xóa thất bại: " + (res?.error || "Lỗi không xác định"));
                   }
                 }
               }}>
                 <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><polyline points="3 6 5 6 21 6"></polyline><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path></svg>
               </button>
            </div>
          </div>
          
          <div style={{overflowX: 'auto', marginTop: '10px'}}>
            <table style={{width:'100%', borderCollapse:'collapse', fontSize:'12px', textAlign:'left'}}>
              <thead>
                <tr style={{borderBottom:'1px solid var(--border)', color:'var(--text-dim)', letterSpacing:'1px', fontFamily:"'Rajdhani',sans-serif"}}>
                  <th style={{padding:'10px 8px'}}>THỜI GIAN</th>
                  <th style={{padding:'10px 8px'}}>BIỂN SỐ</th>
                  <th style={{padding:'10px 8px'}}>CAMERA</th>
                  <th style={{padding:'10px 8px'}}>LOẠI XE</th>
                  <th style={{padding:'10px 8px'}}>MÀU SẮC</th>
                  <th style={{padding:'10px 8px'}}>ĐỘ CHÍNH XÁC (AI)</th>
                </tr>
              </thead>
              <tbody>
                {lprData.map((lpr, idx) => (
                  <tr key={lpr.id || idx} style={{borderBottom:'1px dashed rgba(255,255,255,0.05)'}}>
                    <td style={{padding:'12px 8px', fontFamily:"'Share Tech Mono',monospace", color:'var(--text-secondary)'}}>
                      {lpr.detected_at || lpr.timestamp ? new Date((lpr.detected_at || lpr.timestamp) * 1000).toLocaleString('vi-VN') : '-'}
                    </td>
                    <td style={{padding:'12px 8px'}}>
                      <span style={{background:'rgba(0,200,245,0.1)', color:'var(--accent)', padding:'4px 8px', borderRadius:'3px', fontFamily:"'Share Tech Mono',monospace", fontWeight:700, border:'1px solid var(--accent)'}}>
                        {lpr.plate_number}
                      </span>
                    </td>
                    <td style={{padding:'12px 8px'}}>CAM-{lpr.camera_id || '?'}</td>
                    <td style={{padding:'12px 8px', textTransform:'uppercase'}}>{lpr.vehicle_type || '-'}</td>
                    <td style={{padding:'12px 8px'}}>{lpr.color || '-'}</td>
                    <td style={{padding:'12px 8px', fontFamily:"'Share Tech Mono',monospace", color:'var(--warn)'}}>
                      {lpr.confidence ? `${(lpr.confidence * 100).toFixed(0)}%` : '-'}
                    </td>
                  </tr>
                ))}
                {lprData.length === 0 && (
                  <tr>
                    <td colSpan="6" style={{ textAlign: 'center', padding: '40px', color: 'var(--text-dim)' }}>
                      {loading ? <div className="ai-dot-spin" style={{margin:'0 auto'}}></div> : 'Không có dữ liệu nhận diện biển số nào'}
                    </td>
                  </tr>
                )}
              </tbody>
            </table>
          </div>
        </div>
        
      </div>
    </div>
  );
};

export default AnalyticsView;
