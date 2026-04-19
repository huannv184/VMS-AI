import React, { useState, useEffect, useMemo } from 'react';
import { Calendar, Search, UserCheck, Clock, AlertTriangle, Download, Filter } from 'lucide-react';
import apiClient from '../api/apiClient';

const AttendanceView = () => {
  const [attendanceData, setAttendanceData] = useState([]);
  const [loading, setLoading] = useState(true);
  const [selectedDate, setSelectedDate] = useState(new Date().toISOString().split('T')[0]);
  const [searchQuery, setSearchQuery] = useState('');

  const fetchAttendance = async (date) => {
    setLoading(true);
    try {
      const res = await apiClient.getAttendance(date);
      if (res.success && res.data?.attendance) {
        setAttendanceData(res.data.attendance);
      } else {
        setAttendanceData([]);
      }
    } catch (err) {
      console.error('Failed to fetch attendance', err);
      setAttendanceData([]);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    fetchAttendance(selectedDate);
  }, [selectedDate]);

  // Derived stats
  const stats = useMemo(() => {
    let present = 0;
    let late = 0;
    let onTime = 0;
    
    // Standard rule: 08:00 AM constraint
    const lateThresholdHours = 8;
    const lateThresholdMinutes = 15;

    attendanceData.forEach(record => {
      present++;
      const checkInDate = new Date(record.check_in);
      if (checkInDate.getHours() > lateThresholdHours || (checkInDate.getHours() === lateThresholdHours && checkInDate.getMinutes() > lateThresholdMinutes)) {
        late++;
      } else {
        onTime++;
      }
    });

    return { total: attendanceData.length, present, late, onTime };
  }, [attendanceData]);

  const filteredData = useMemo(() => {
    if (!searchQuery) return attendanceData;
    const q = searchQuery.toLowerCase();
    return attendanceData.filter(item => item.person_name && item.person_name.toLowerCase().includes(q));
  }, [attendanceData, searchQuery]);

  return (
    <div className="analytics-view active" style={{ padding: '20px', overflowY: 'auto' }}>
      <div className="analytics-inner" style={{ maxWidth: '1200px', margin: '0 auto' }}>
        
        {/* Header */}
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 20 }}>
          <div style={{ fontFamily: 'Rajdhani', fontWeight: 700, fontSize: 24, letterSpacing: 2, textTransform: 'uppercase', color: 'var(--accent)' }}>
            Hệ Thống Chấm Công AI
          </div>
          <div style={{ display: 'flex', gap: 10, alignItems: 'center' }}>
            <div style={{ display: 'flex', alignItems: 'center', background: 'var(--bg)', border: '1px solid var(--border)', borderRadius: '4px', padding: '0 10px' }}>
              <input 
                 type="date" 
                 value={selectedDate}
                 onChange={e => setSelectedDate(e.target.value)}
                 style={{ background: 'transparent', border: 'none', color: 'var(--text)', padding: '6px 0', outline: 'none', fontFamily:'Share Tech Mono' }}
              />
            </div>
            <button className="pb-btn" style={{ padding: '6px 12px', background: 'var(--bg)', border: '1px solid var(--border)' }}>
              <Download size={14} style={{ marginRight: 6 }} /> Xuất Excel
            </button>
            <button className="pb-btn" style={{ padding: '6px 12px', background: 'var(--accent)', color: '#000', fontWeight: 'bold' }} onClick={() => fetchAttendance(selectedDate)}>
              <Calendar size={14} style={{ marginRight: 6 }} /> Làm mới
            </button>
          </div>
        </div>

        {/* Dashboard Stats */}
        <div className="analytics-grid" style={{ marginBottom: 20, gridTemplateColumns: 'repeat(4, 1fr)' }}>
          <div className="analytics-card glass" style={{ display: 'flex', alignItems: 'center', gap: 15 }}>
            <div style={{ width: 50, height: 50, borderRadius: '50%', background: 'rgba(0,200,245,0.1)', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
               <UserCheck size={24} color="var(--accent)" />
            </div>
            <div>
               <div style={{ fontSize: 11, color: 'var(--text-secondary)', textTransform: 'uppercase' }}>Tổng lượt diện diện</div>
               <div style={{ fontFamily: 'Rajdhani', fontWeight: 700, fontSize: 28, color: 'var(--accent)' }}>{stats.total}</div>
            </div>
          </div>

          <div className="analytics-card glass" style={{ display: 'flex', alignItems: 'center', gap: 15 }}>
            <div style={{ width: 50, height: 50, borderRadius: '50%', background: 'rgba(50,232,39,0.1)', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
               <Clock size={24} color="var(--accent3)" />
            </div>
            <div>
               <div style={{ fontSize: 11, color: 'var(--text-secondary)', textTransform: 'uppercase' }}>ĐÚNG GIỜ (Trước 8h15)</div>
               <div style={{ fontFamily: 'Rajdhani', fontWeight: 700, fontSize: 28, color: 'var(--accent3)' }}>{stats.onTime}</div>
            </div>
          </div>

          <div className="analytics-card glass" style={{ display: 'flex', alignItems: 'center', gap: 15 }}>
            <div style={{ width: 50, height: 50, borderRadius: '50%', background: 'rgba(255,184,0,0.1)', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
               <AlertTriangle size={24} color="var(--warn)" />
            </div>
            <div>
               <div style={{ fontSize: 11, color: 'var(--text-secondary)', textTransform: 'uppercase' }}>ĐI MUỘN</div>
               <div style={{ fontFamily: 'Rajdhani', fontWeight: 700, fontSize: 28, color: 'var(--warn)' }}>{stats.late}</div>
            </div>
          </div>
          
          <div className="analytics-card glass" style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', padding: '15px' }}>
             <div style={{ textAlign: 'center' }}>
               <div style={{ fontSize: 10, color: 'var(--text-dim)', marginBottom: 5 }}>RULE CA SÁNG</div>
               <div style={{ fontFamily: 'Share Tech Mono', fontSize: 14, color: 'var(--text)', background: 'rgba(255,255,255,0.05)', padding: '5px 15px', borderRadius: 4, display: 'inline-block' }}>
                 08:00 - 17:00
               </div>
             </div>
          </div>
        </div>

        {/* Main Attendance Table */}
        <div className="analytics-card glass">
          <div className="analytics-card-title" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
            <span>Chi Tiết Điểm Danh Ngày {selectedDate.split('-').reverse().join('/')}</span>
            <div style={{ display: 'flex', gap: 8 }}>
               <div style={{ position: 'relative' }}>
                 <Search size={14} style={{ position: 'absolute', left: 10, top: 7, color: 'var(--text-dim)' }} />
                 <input 
                   className="config-input" 
                   placeholder="Tìm tên nhân sự..." 
                   value={searchQuery}
                   onChange={e => setSearchQuery(e.target.value)}
                   style={{ width: 200, padding: '5px 10px 5px 30px', fontSize: '12px' }} 
                 />
               </div>
               <button className="grid-btn"><Filter size={14} /></button>
            </div>
          </div>
          
          <div style={{ overflowX: 'auto', marginTop: '15px' }}>
            <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: '13px', textAlign: 'left' }}>
              <thead>
                <tr style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-dim)', letterSpacing: '1px', fontFamily: "'Rajdhani', sans-serif" }}>
                  <th style={{ padding: '12px' }}>NHÂN SỰ (FACE_ID)</th>
                  <th style={{ padding: '12px' }}>GIỜ VÀO (CHECK-IN)</th>
                  <th style={{ padding: '12px' }}>GIỜ RA (CHECK-OUT)</th>
                  <th style={{ padding: '12px' }}>THỜI GIAN HIỆN DIỆN</th>
                  <th style={{ padding: '12px' }}>TRẠNG THÁI</th>
                </tr>
              </thead>
              <tbody>
                {filteredData.map((record, idx) => {
                   const checkInDate = new Date(record.check_in);
                   const checkOutDate = new Date(record.check_out);
                   
                   // Late calc
                   const lateThresholdHours = 8;
                   const lateThresholdMinutes = 15;
                   const isLate = checkInDate.getHours() > lateThresholdHours || (checkInDate.getHours() === lateThresholdHours && checkInDate.getMinutes() > lateThresholdMinutes);
                   
                   // Work hours duration calc
                   let diffMs = checkOutDate.getTime() - checkInDate.getTime();
                   const diffHours = Math.floor(diffMs / 3600000);
                   const diffMins = Math.floor((diffMs % 3600000) / 60000);
                   
                   let statusLabel = isLate ? 'Đi Muộn' : 'Đúng Giờ';
                   let statusClass = isLate ? 'warn' : 'active';
                   
                   if (diffMs === 0) {
                      statusLabel = 'Thiếu Check-out';
                      statusClass = 'danger';
                   }

                   return (
                    <tr key={idx} style={{ borderBottom: '1px solid rgba(255,255,255,0.02)', transition: 'background 0.2s' }}>
                      <td style={{ padding: '12px', fontWeight: 700, color: 'var(--text)' }}>
                         {record.person_name}
                      </td>
                      <td style={{ padding: '12px', fontFamily: "'Share Tech Mono', monospace" }}>
                         {checkInDate.toLocaleTimeString('vi-VN', { hour: '2-digit', minute: '2-digit', second: '2-digit' })}
                      </td>
                      <td style={{ padding: '12px', fontFamily: "'Share Tech Mono', monospace", color: diffMs === 0 ? 'var(--text-dim)' : 'var(--text)' }}>
                         {diffMs === 0 ? '--:--:--' : checkOutDate.toLocaleTimeString('vi-VN', { hour: '2-digit', minute: '2-digit', second: '2-digit' })}
                      </td>
                      <td style={{ padding: '12px', fontFamily: "'Rajdhani', monospace", fontWeight: 600 }}>
                         {diffMs === 0 ? '0h 0m' : `${diffHours}h ${diffMins}m`}
                      </td>
                      <td style={{ padding: '12px' }}>
                        <span className={`config-badge ${statusClass}`} style={{ 
                           border: `1px solid var(--${statusClass})`, 
                           color: `var(--${statusClass})`, 
                           background: `rgba(0,0,0,0.3)` 
                        }}>
                           {statusLabel}
                        </span>
                      </td>
                    </tr>
                  )
                })}
                {filteredData.length === 0 && !loading && (
                  <tr>
                    <td colSpan="5" style={{ textAlign: 'center', padding: '50px', color: 'var(--text-dim)' }}>
                      Không có lịch sử điểm danh nhân sự nào trong Ngày {selectedDate.split('-').reverse().join('/')}
                    </td>
                  </tr>
                )}
                {loading && (
                  <tr>
                    <td colSpan="5" style={{ textAlign: 'center', padding: '50px' }}>
                      <div className="ai-dot-spin" style={{ margin: '0 auto' }}></div>
                      <div style={{ marginTop: 10, color: 'var(--accent)', fontSize: 12 }}>ĐANG QUÉT DỮ LIỆU...</div>
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

export default AttendanceView;
