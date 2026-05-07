import React, { useState, useEffect, useMemo } from 'react';
import { Calendar, Search, UserCheck, Clock, AlertTriangle, Download, Filter, Settings } from 'lucide-react';
import apiClient from '../api/apiClient';
import useVmsStore from '../store/useVmsStore';
import AttendanceSettings from '../components/AttendanceSettings';

const STATUS_FILTERS = [
  { value: 'all', label: 'Tất cả' },
  { value: 'on_time', label: 'Đúng giờ' },
  { value: 'late', label: 'Đi muộn' },
  { value: 'no_checkout', label: 'Thiếu Check-out' },
];

const AttendanceView = () => {
  const [attendanceData, setAttendanceData] = useState([]);
  const [loading, setLoading] = useState(true);
  const [selectedDate, setSelectedDate] = useState(new Date().toISOString().split('T')[0]);
  const [searchQuery, setSearchQuery] = useState('');
  const [statusFilter, setStatusFilter] = useState('all');
  const [showStatusFilter, setShowStatusFilter] = useState(false);
  const [showSettings, setShowSettings] = useState(false);
  const [toast, setToast] = useState(null);
  const cameras = useVmsStore((state) => state.cameras);

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

  // Server already produces a richer CSV (Mã NV, Ca, Trễ, Trạng thái, …) with
  // identical late-minute logic to the rollup. Defer to it instead of
  // re-computing client-side — that's how a hardcoded 8:15 rule slipped in.
  const handleExport = async () => {
    if (attendanceData.length === 0) {
      alert('Không có dữ liệu để xuất.');
      return;
    }
    try {
      const result = await apiClient.exportAttendanceCsv(selectedDate);
      if (!apiClient.saveBlobResponse(result, `attendance_${selectedDate}.csv`)) {
        alert('Xuất CSV thất bại: ' + (result?.error || 'phản hồi không hợp lệ'));
      }
    } catch (e) {
      alert('Xuất CSV thất bại: ' + (e?.message || 'Lỗi mạng'));
    }
  };

  useEffect(() => {
    fetchAttendance(selectedDate);
  }, [selectedDate]);

  // Derived stats — uses backend-computed is_late which honours each
  // employee's assigned shift (start_time + grace_min). Records with no shift
  // assignment have is_late=null and are bucketed as "unscheduled".
  const stats = useMemo(() => {
    let late = 0;
    let onTime = 0;
    let unscheduled = 0;
    attendanceData.forEach(record => {
      if (record.is_late === null || record.is_late === undefined) {
        unscheduled++;
      } else if (record.is_late) {
        late++;
      } else {
        onTime++;
      }
    });
    return { total: attendanceData.length, present: attendanceData.length, late, onTime, unscheduled };
  }, [attendanceData]);

  const filteredData = useMemo(() => {
    const q = searchQuery.toLowerCase();
    return attendanceData.filter(item => {
      if (q && !(item.person_name && item.person_name.toLowerCase().includes(q))) return false;
      if (statusFilter !== 'all') {
        const ci = new Date(item.check_in);
        const co = new Date(item.check_out);
        const diffMs = co.getTime() - ci.getTime();
        const isLate = !!item.is_late;
        if (statusFilter === 'on_time' && (isLate || diffMs === 0)) return false;
        if (statusFilter === 'late' && (!isLate || diffMs === 0)) return false;
        if (statusFilter === 'no_checkout' && diffMs !== 0) return false;
      }
      return true;
    });
  }, [attendanceData, searchQuery, statusFilter]);

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
            <button className="pb-btn" style={{ padding: '6px 12px', background: 'var(--bg)', border: '1px solid var(--border)' }} onClick={handleExport}>
              <Download size={14} style={{ marginRight: 6 }} /> Xuất CSV
            </button>
            <button className="pb-btn" style={{ padding: '6px 12px', background: 'var(--bg)', border: '1px solid var(--border)' }} onClick={() => setShowSettings((v) => !v)}>
              <Settings size={14} style={{ marginRight: 6 }} /> {showSettings ? 'Đóng cấu hình' : 'Cấu hình'}
            </button>
            <button className="pb-btn" style={{ padding: '6px 12px', background: 'var(--accent)', color: '#000', fontWeight: 'bold' }} onClick={() => fetchAttendance(selectedDate)}>
              <Calendar size={14} style={{ marginRight: 6 }} /> Làm mới
            </button>
          </div>
        </div>

        {showSettings && (
          <div className="analytics-card glass" style={{ marginBottom: 20, padding: 16 }}>
            <div className="analytics-card-title" style={{ marginBottom: 12 }}>
              Cấu hình Chấm công · Nhân viên · Vai trò Camera
            </div>
            <AttendanceSettings
              cameras={cameras}
              onToast={(msg, level = 'info') => {
                setToast({ msg, level });
                setTimeout(() => setToast(null), 3500);
              }}
            />
          </div>
        )}

        {toast && (
          <div style={{
            position: 'fixed', right: 24, bottom: 24, zIndex: 1100,
            padding: '10px 16px', borderRadius: 6, fontSize: 12,
            background: 'rgba(0,0,0,0.85)',
            border: `1px solid var(--${toast.level === 'error' ? 'danger' : toast.level === 'warn' ? 'warn' : 'accent3'})`,
            color: `var(--${toast.level === 'error' ? 'danger' : toast.level === 'warn' ? 'warn' : 'accent3'})`,
          }}>
            {toast.msg}
          </div>
        )}

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
               <div style={{ fontSize: 11, color: 'var(--text-secondary)', textTransform: 'uppercase' }}>ĐÚNG GIỜ (theo ca)</div>
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
               <div style={{ fontSize: 10, color: 'var(--text-dim)', marginBottom: 5 }}>CHƯA GÁN CA</div>
               <div style={{ fontFamily: 'Rajdhani', fontWeight: 700, fontSize: 28, color: 'var(--text-dim)' }}>
                 {stats.unscheduled}
               </div>
               <div style={{ fontSize: 10, color: 'var(--text-dim)', marginTop: 4 }}>
                 mở "Cấu hình" → gán ca cho NV
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
               <div style={{ position: 'relative' }}>
                 <button
                   className="grid-btn"
                   style={{ background: statusFilter !== 'all' ? 'rgba(0,200,245,0.15)' : undefined, color: statusFilter !== 'all' ? 'var(--accent)' : undefined }}
                   onClick={() => setShowStatusFilter((v) => !v)}
                   title="Lọc theo trạng thái"
                 >
                   <Filter size={14} /> {STATUS_FILTERS.find(s => s.value === statusFilter)?.label}
                 </button>
                 {showStatusFilter && (
                   <div style={{ position: 'absolute', right: 0, top: '110%', background: 'var(--bg-panel)', border: '1px solid var(--border)', borderRadius: 4, zIndex: 10, minWidth: 140 }}>
                     {STATUS_FILTERS.map((s) => (
                       <div
                         key={s.value}
                         style={{ padding: '8px 12px', cursor: 'pointer', fontSize: 12, color: statusFilter === s.value ? 'var(--accent)' : 'var(--text)' }}
                         onClick={() => { setStatusFilter(s.value); setShowStatusFilter(false); }}
                       >
                         {statusFilter === s.value ? '● ' : '  '}{s.label}
                       </div>
                     ))}
                   </div>
                 )}
               </div>
            </div>
          </div>
          
          <div style={{ overflowX: 'auto', marginTop: '15px' }}>
            <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: '13px', textAlign: 'left' }}>
              <thead>
                <tr style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-dim)', letterSpacing: '1px', fontFamily: "'Rajdhani', sans-serif" }}>
                  <th style={{ padding: '12px' }}>NHÂN SỰ (FACE_ID)</th>
                  <th style={{ padding: '12px' }}>CA</th>
                  <th style={{ padding: '12px' }}>GIỜ VÀO (CHECK-IN)</th>
                  <th style={{ padding: '12px' }}>GIỜ RA (CHECK-OUT)</th>
                  <th style={{ padding: '12px' }}>THỜI GIAN HIỆN DIỆN</th>
                  <th style={{ padding: '12px' }}>OT</th>
                  <th style={{ padding: '12px' }}>TRẠNG THÁI</th>
                </tr>
              </thead>
              <tbody>
                {filteredData.map((record, idx) => {
                   const checkInDate = new Date(record.check_in);
                   const checkOutDate = new Date(record.check_out);

                   const diffMs = checkOutDate.getTime() - checkInDate.getTime();
                   const diffHours = Math.floor(diffMs / 3600000);
                   const diffMins = Math.floor((diffMs % 3600000) / 60000);

                   // Status priority: missing checkout > severe-late > late > on-time > unscheduled.
                   // is_late / is_late_severe come from backend (per-employee shift); null when
                   // no shift is assigned to the employee.
                   const hasShift = record.is_late !== null && record.is_late !== undefined;
                   let statusLabel, statusClass;
                   if (diffMs === 0) {
                     statusLabel = 'Thiếu Check-out';
                     statusClass = 'danger';
                   } else if (!hasShift) {
                     statusLabel = 'Chưa gán ca';
                     statusClass = 'inactive';
                   } else if (record.is_late_severe) {
                     statusLabel = `Trễ nặng (${record.late_minutes}p)`;
                     statusClass = 'danger';
                   } else if (record.is_late) {
                     statusLabel = `Trễ ${record.late_minutes}p`;
                     statusClass = 'warn';
                   } else {
                     statusLabel = 'Đúng giờ';
                     statusClass = 'active';
                   }

                   return (
                    <tr key={idx} style={{ borderBottom: '1px solid rgba(255,255,255,0.02)', transition: 'background 0.2s' }}>
                      <td style={{ padding: '12px', fontWeight: 700, color: 'var(--text)' }}>
                         {record.person_name}
                      </td>
                      <td style={{ padding: '12px', fontFamily: "'Share Tech Mono', monospace", fontSize: 11 }}>
                         {hasShift ? (
                           <span>
                             <span style={{ color: 'var(--accent3)' }}>{record.shift_name}</span>
                             <span style={{ color: 'var(--text-dim)', marginLeft: 6 }}>
                               {record.shift_start}-{record.shift_end}
                             </span>
                           </span>
                         ) : <span style={{ color: 'var(--text-dim)' }}>—</span>}
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
                      <td style={{ padding: '12px', fontFamily: "'Share Tech Mono', monospace", fontSize: 11 }}>
                         {(() => {
                           // overtime_minutes is null when no shift assigned, or
                           // when worker checked out within shift_end + ot_grace.
                           // Show "—" for nullable, "—" for 0, formatted Xh Ym for >0.
                           const ot = record.overtime_minutes;
                           if (ot === null || ot === undefined || ot === 0) {
                             return <span style={{ color: 'var(--text-dim)' }}>—</span>;
                           }
                           const oh = Math.floor(ot / 60);
                           const om = ot % 60;
                           return <span style={{ color: 'var(--accent)' }}>{oh > 0 ? `${oh}h ${om}m` : `${om}m`}</span>;
                         })()}
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
                    <td colSpan="7" style={{ textAlign: 'center', padding: '50px', color: 'var(--text-dim)' }}>
                      Không có lịch sử điểm danh nhân sự nào trong Ngày {selectedDate.split('-').reverse().join('/')}
                    </td>
                  </tr>
                )}
                {loading && (
                  <tr>
                    <td colSpan="7" style={{ textAlign: 'center', padding: '50px' }}>
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
