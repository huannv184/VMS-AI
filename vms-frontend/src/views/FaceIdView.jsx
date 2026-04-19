import React, { useState, useEffect, useRef } from 'react';
import apiClient from '../api/apiClient';

const API_BASE = ''; // Vite proxy forward đến :8000
const normalizeTimestamp = (value) => (value > 9999999999 ? value : value * 1000);

const FaceIdView = () => {
  const [activeTab, setActiveTab] = useState('VERIFIED'); // VERIFIED, UNKNOWN, ADD_NEW
  const [persons, setPersons] = useState([]);
  const [recognitionLogs, setRecognitionLogs] = useState([]);
  const [selectedPerson, setSelectedPerson] = useState(null);
  
  // Add new form state
  const [newName, setNewName] = useState('');
  const [newDesc, setNewDesc] = useState('');
  const [previewImage, setPreviewImage] = useState(null);
  const [isAdding, setIsAdding] = useState(false);
  
  const fileInputRef = useRef(null);

  // Fetch initial data
  const fetchData = async () => {
    try {
      const pRes = await apiClient.getPersons();
      if (pRes.success) setPersons(pRes.data?.persons || []);
      
      const evtRes = await apiClient.getEvents({ event_type: 'FACE_RECOGNIZED', limit: 20 });
      if (evtRes.success) {
        setRecognitionLogs(evtRes.data?.events || []);
      }
    } catch (e) {
      console.error("Failed to fetch faces", e);
    }
  };

  useEffect(() => {
    fetchData();
    const intv = setInterval(() => {
      apiClient.getEvents({ event_type: 'FACE_RECOGNIZED', limit: 20 })
      .then((res) => { if (res.success) setRecognitionLogs(res.data?.events || []); })
      .catch(()=>{});
    }, 5000);
    return () => clearInterval(intv);
  }, []);

  const handleImageSelect = (e) => {
    const file = e.target.files[0];
    if (file) {
      if (file.size > 10 * 1024 * 1024) {
        alert("File quá lớn! Giới hạn 10MB");
        return;
      }
      const reader = new FileReader();
      reader.onloadend = () => setPreviewImage(reader.result);
      reader.readAsDataURL(file);
    }
  };

  const handleAddPerson = async () => {
    if (!newName.trim() || !previewImage) {
      alert("Vui lòng nhập tên và chọn hình ảnh!");
      return;
    }
    
    setIsAdding(true);
    try {
      // Remove data:image... block
      const base64Data = previewImage.split(',')[1];
      
      const res = await apiClient.addPerson({
        name: newName,
        description: newDesc || 'Nhân viên',
        image: base64Data
      });
      
      if (res.success) {
        setNewName('');
        setNewDesc('');
        setPreviewImage(null);
        setActiveTab('VERIFIED');
        fetchData(); // Reload persons
      } else {
        alert(res.error || "Thêm thất bại. Có thể AI Worker đang không phản hồi.");
      }
    } catch(e) {
      alert("Lỗi kết nối");
    } finally {
      setIsAdding(false);
    }
  };

  const handleDelete = async (id) => {
    if (!window.confirm("Bạn có chắc chắn muốn xóa khuôn mặt này?")) return;
    try {
      await apiClient.deletePerson(id);
      setSelectedPerson(null);
      fetchData();
    } catch(e) {
      alert("Xóa thất bại!");
    }
  };

  const formatTime = (ts) => {
    if (!ts) return '';
    return new Date(normalizeTimestamp(ts)).toLocaleTimeString();
  };

  return (
    <div className="view-panel active">
      <div className="grid-toolbar">
        <div className={`grid-btn ${activeTab === 'VERIFIED' ? 'active' : ''}`} onClick={() => {setActiveTab('VERIFIED'); setSelectedPerson(null);}}>Đã xác minh</div>
        <div className={`grid-btn ${activeTab === 'UNKNOWN' ? 'active' : ''}`} onClick={() => {setActiveTab('UNKNOWN'); setSelectedPerson(null);}}>Lạ mặt</div>
        <div className={`grid-btn ${activeTab === 'ADD_NEW' ? 'active' : ''}`} onClick={() => setActiveTab('ADD_NEW')} style={{background: activeTab === 'ADD_NEW' ? 'rgba(0,200,245,0.2)' : ''}}>+ Thêm mới</div>
        <div className="toolbar-sep"></div>
        <div className="ai-badge"><div className="ai-dot-spin"></div>Face AI · Active</div>
      </div>
      
      <div style={{display:'flex', flex:1, overflow:'hidden', minHeight:0}}>
        
        {/* CENTER CONTENT */}
        <div style={{flex:1, overflowY:'auto', padding:'12px'}} id="faceGrid">
          
          {/* TAB: VERIFIED */}
          {activeTab === 'VERIFIED' && (
            <div style={{display:'grid', gridTemplateColumns:'repeat(auto-fill, minmax(140px, 1fr))', gap:'12px', alignContent:'start'}}>
              {persons.map((person) => {
                let tagColor = 'var(--accent)';
                let tagBg = 'rgba(0,200,245,0.15)';
                const role = person.description || 'Nhân viên';
                if (role.toUpperCase().includes('VIP')) { tagColor = 'var(--warn)'; tagBg = 'rgba(255,184,0,0.15)'; }
                if (role.toUpperCase().includes('BLACKLIST')) { tagColor = 'var(--danger)'; tagBg = 'rgba(255,48,96,0.15)'; }
                
                return (
                  <div key={person.id} onClick={() => setSelectedPerson(person)} style={{background:'var(--bg-card)', border:`1px solid ${selectedPerson?.id === person.id ? tagColor : 'var(--border)'}`, borderRadius:'4px', overflow:'hidden', cursor:'pointer', transition:'border 0.2s'}}>
                    <div style={{height:'140px', background:'#111', display:'flex', alignItems:'center', justifyContent:'center', position:'relative', overflow:'hidden'}}>
                      {person.face_image_path ? (
                        <img src={`${API_BASE}${person.face_image_path}`} alt={person.name} style={{ width:'100%', height:'100%', objectFit:'cover' }} onError={(e) => { e.target.style.display = 'none'; }} />
                      ) : (
                        <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="var(--text-dim)" strokeWidth="1.5"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>
                      )}
                      {person.has_embedding && (
                        <div style={{position:'absolute', top:'5px', right:'5px', background:'rgba(0,200,100,0.8)', padding:'2px 4px', borderRadius:'2px', fontFamily:"'Share Tech Mono',monospace", fontSize:'10px', color:'#fff', display:'flex', alignItems:'center', gap:'3px'}}>
                          <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><polyline points="20 6 9 17 4 12"></polyline></svg> AI
                        </div>
                      )}
                    </div>
                    <div style={{padding:'10px'}}>
                      <div style={{fontFamily:"'Rajdhani',sans-serif", fontWeight:700, fontSize:'13px', color:'var(--text-primary)', letterSpacing:'1px', whiteSpace:'nowrap', overflow:'hidden', textOverflow:'ellipsis'}}>{person.name}</div>
                      <div style={{display:'flex', justifyContent:'space-between', alignItems:'center', marginTop:'6px'}}>
                        <span style={{fontSize:'9px', padding:'2px 6px', borderRadius:'2px', background:tagBg, color:tagColor, border:`1px solid ${tagColor}`, fontFamily:"'Share Tech Mono',monospace"}}>{role}</span>
                      </div>
                    </div>
                  </div>
                );
              })}
              {persons.length === 0 && (
                <div style={{gridColumn: '1 / -1', textAlign:'center', padding:'60px 20px', color:'var(--text-dim)'}}>
                  <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="var(--text-dim)" strokeWidth="1" style={{marginBottom: 15, opacity: 0.3}}><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>
                  <div style={{fontSize: 13, fontWeight: 600, letterSpacing: 1}}>CHƯA CÓ NGƯỜI ĐĂNG KÝ</div>
                  <div style={{fontSize: 10, marginTop: 5}}>Nhấn vào tab Thêm Mới để bắt đầu</div>
                </div>
              )}
            </div>
          )}
          
          {/* TAB: UNKNOWN */}
          {activeTab === 'UNKNOWN' && (
             <div style={{textAlign:'center', padding:'60px 20px', color:'var(--text-dim)'}}>
                <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="var(--text-dim)" strokeWidth="1" style={{marginBottom: 15, opacity: 0.3}}><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"></path><circle cx="12" cy="12" r="3"></circle></svg>
                <div style={{fontSize: 13, fontWeight: 600, letterSpacing: 1}}>KHÔNG TÌM THẤY NGƯỜI LẠ MẶT DỮ LIỆU CŨ</div>
             </div>
          )}

          {/* TAB: ADD NEW */}
          {activeTab === 'ADD_NEW' && (
            <div style={{maxWidth: '500px', margin: '0 auto', background: 'var(--bg-card)', border: '1px solid var(--border)', borderRadius: '6px', padding: '20px'}}>
               <div style={{fontSize:'14px', letterSpacing:'1.5px', fontFamily:"'Rajdhani',sans-serif", fontWeight:700, color:'var(--accent)', marginBottom:'20px', borderBottom:'1px solid var(--border)', paddingBottom:'10px'}}>
                 ĐĂNG KÝ KHUÔN MẶT MỚI
               </div>
               
               <div style={{display:'flex', gap:'15px', marginBottom:'20px'}}>
                  {/* Image Preview / Upload Box */}
                  <div 
                    onClick={() => fileInputRef.current?.click()}
                    style={{width:'120px', height:'120px', borderRadius:'4px', border:'2px dashed var(--border)', background:'var(--bg)', display:'flex', flexDirection:'column', alignItems:'center', justifyContent:'center', cursor:'pointer', overflow:'hidden', flexShrink:0, transition:'border 0.2s', position:'relative'}}
                  >
                    {previewImage ? (
                      <img src={previewImage} alt="Preview" style={{width:'100%', height:'100%', objectFit:'cover'}} />
                    ) : (
                      <>
                        <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="var(--text-dim)" strokeWidth="1.5" style={{marginBottom:'5px'}}><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/></svg>
                        <div style={{fontSize:'10px', color:'var(--text-dim)'}}>Tải ảnh lên</div>
                      </>
                    )}
                    <input type="file" accept="image/*" ref={fileInputRef} onChange={handleImageSelect} style={{display:'none'}} />
                  </div>
                  
                  {/* Inputs */}
                  <div style={{flex:1, display:'flex', flexDirection:'column', gap:'12px'}}>
                    <div>
                      <div style={{fontSize:'10px', color:'var(--text-secondary)', marginBottom:'4px'}}>Họ và Tên *</div>
                      <input 
                        type="text" 
                        value={newName} 
                        onChange={e => setNewName(e.target.value)} 
                        style={{width:'100%', background:'var(--bg)', border:'1px solid var(--border)', color:'var(--text)', padding:'8px 10px', borderRadius:'3px', fontFamily:"'Rajdhani',sans-serif", fontSize:'13px', outline:'none'}} 
                        placeholder="Nhập tên..." 
                      />
                    </div>
                    <div>
                      <div style={{fontSize:'10px', color:'var(--text-secondary)', marginBottom:'4px'}}>Phân quyền / Ghi chú</div>
                      <input 
                        type="text" 
                        value={newDesc} 
                        onChange={e => setNewDesc(e.target.value)} 
                        style={{width:'100%', background:'var(--bg)', border:'1px solid var(--border)', color:'var(--text)', padding:'8px 10px', borderRadius:'3px', fontFamily:"'Rajdhani',sans-serif", fontSize:'13px', outline:'none'}} 
                        placeholder="VD: VIP, Nhân viên, Blacklist..." 
                      />
                    </div>
                  </div>
               </div>
               
               <div style={{display:'flex', justifyContent:'flex-end', gap:'10px', borderTop:'1px solid var(--border)', paddingTop:'15px'}}>
                  <div className="grid-btn" onClick={() => { setActiveTab('VERIFIED'); setNewName(''); setPreviewImage(null); }}>Hủy</div>
                  <div 
                    className="grid-btn" 
                    style={{background: 'var(--accent)', color: '#000', fontWeight: 'bold', display: 'flex', alignItems: 'center', gap: '6px', pointerEvents: isAdding ? 'none' : 'auto', opacity: isAdding ? 0.7 : 1}} 
                    onClick={handleAddPerson}
                  >
                    {isAdding ? <div className="ai-dot-spin" style={{borderColor:'#000', borderTopColor:'transparent', width:'12px', height:'12px'}}></div> : <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"></path><polyline points="17 21 17 13 7 13 7 21"></polyline><polyline points="7 3 7 8 15 8"></polyline></svg>}
                    {isAdding ? 'Đang trích xuất AI...' : 'Lưu Khuôn Mặt'}
                  </div>
               </div>
            </div>
          )}
        </div>
        
        {/* RIGHT SIDE PANEL */}
        <div style={{width:'260px', flexShrink:0, borderLeft:'1px solid var(--border)', background:'var(--bg-panel)', display:'flex', flexDirection:'column', overflowY:'auto'}}>
          
          {/* Detail Object Data */}
          <div style={{padding:'10px 12px', borderBottom:'1px solid var(--border)', fontFamily:"'Rajdhani',sans-serif", fontWeight:700, fontSize:'11px', letterSpacing:'1.5px', color:'var(--text-dim)', background:'var(--bg-card)'}}>CHI TIẾT ĐỐI TƯỢNG</div>
          <div style={{padding:'12px', flex:1}}>
            {selectedPerson ? (() => {
              let tagColor = 'var(--accent)';
              const role = selectedPerson.description || 'Nhân viên';
              if (role.toUpperCase().includes('VIP')) tagColor = 'var(--warn)';
              if (role.toUpperCase().includes('BLACKLIST')) tagColor = 'var(--danger)';
              
              return (
                <>
                  <div style={{textAlign:'center', marginBottom:'15px'}}>
                    <div style={{width:'100px', height:'100px', borderRadius:'50%', background:'#111', border:`2px solid ${tagColor}`, margin:'0 auto 10px', display:'flex', alignItems:'center', justifyContent:'center', overflow:'hidden'}}>
                      {selectedPerson.face_image_path ? (
                        <img src={`${API_BASE}${selectedPerson.face_image_path}`} alt={selectedPerson.name} style={{ width:'100%', height:'100%', objectFit:'cover' }} />
                      ) : (
                        <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke={tagColor} strokeWidth="1.5"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>
                      )}
                    </div>
                    <div style={{fontFamily:"'Rajdhani',sans-serif", fontWeight:700, fontSize:'18px', color:'var(--text-primary)', letterSpacing:'1px'}}>{selectedPerson.name}</div>
                    <div style={{fontSize:'12px', fontFamily:"'Share Tech Mono',monospace", color:tagColor, marginTop:'4px'}}>{role}</div>
                  </div>
                  
                  <div style={{background:'var(--bg)', padding:'10px', borderRadius:'4px', border:'1px solid var(--border)', marginBottom:'15px'}}>
                    <div style={{display:'flex', justifyContent:'space-between', marginBottom:'8px'}}>
                      <span style={{fontSize:'10px', color:'var(--text-secondary)'}}>Trạng thái AI</span>
                      <span style={{fontSize:'10px', color: selectedPerson.has_embedding ? 'var(--accent)' : 'var(--warn)', fontFamily:"'Share Tech Mono',monospace"}}>
                        {selectedPerson.has_embedding ? 'Đã huấn luyện' : 'Thiếu Embedding'}
                      </span>
                    </div>
                    <div style={{display:'flex', justifyContent:'space-between', marginBottom:'8px'}}>
                      <span style={{fontSize:'10px', color:'var(--text-secondary)'}}>Ngày tạo</span>
                      <span style={{fontSize:'10px', color:'var(--text-primary)', fontFamily:"'Share Tech Mono',monospace"}}>{new Date(selectedPerson.created_at * 1000).toLocaleDateString()}</span>
                    </div>
                    <div style={{display:'flex', justifyContent:'space-between'}}>
                      <span style={{fontSize:'10px', color:'var(--text-secondary)'}}>ID Hệ thống</span>
                      <span style={{fontSize:'10px', color:'var(--text-primary)', fontFamily:"'Share Tech Mono',monospace"}}>PF-{selectedPerson.id}</span>
                    </div>
                  </div>
                  
                  <div 
                    onClick={() => handleDelete(selectedPerson.id)}
                    style={{width:'100%', textAlign:'center', padding:'8px 0', border:'1px solid var(--danger)', color:'var(--danger)', borderRadius:'4px', fontSize:'11px', fontWeight:600, cursor:'pointer', hover:{background:'var(--danger)'}}}
                  >
                    XÓA KHUÔN MẶT NÀY
                  </div>
                </>
              );
            })() : (
              <div style={{textAlign:'center', color:'var(--text-dim)', fontSize:'11px', marginTop:'40px'}}>
                 <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="var(--text-dim)" strokeWidth="1" style={{opacity: 0.3, marginBottom:10}}><rect x="3" y="3" width="18" height="18" rx="2" ry="2"/><circle cx="8.5" cy="8.5" r="1.5"/><polyline points="21 15 16 10 5 21"/></svg>
                 <br/>
                 Click vào 1 nhân viên để<br/>xem thông tin chi tiết
              </div>
            )}
          </div>
          
          {/* Recognition Logs */}
          <div style={{padding:'0', borderTop:'1px solid var(--border)', background: 'var(--bg-card)'}}>
            <div style={{padding:'10px 12px', fontSize:'10px', letterSpacing:'1.5px', fontFamily:"'Rajdhani',sans-serif", fontWeight:700, color:'var(--text-primary)', borderBottom: '1px solid var(--border)', display:'flex', justifyContent:'space-between'}}>
               <span>LOG NHẬN DIỆN THỰC TẾ</span>
               <span style={{color: 'var(--accent)'}}>LIVE</span>
            </div>
            <div style={{display:'flex', flexDirection:'column', gap:'0', height:'300px', overflowY:'auto'}}>
               {recognitionLogs.map((log) => {
                 const desc = log.description || '';
                 const simMatch = desc.match(/similarity:\s*([0-9.]+)%/i); // E.g., "Recognized John Doe (similarity: 95.2%)"
                 const similarity = simMatch ? simMatch[1] + '%' : '99%';
                 // Extract person name from description roughly
                 let pName = "Unknown";
                 if (desc.startsWith("Recognized ")) pName = desc.split(" (")[0].replace("Recognized ", "");
                 
                 return (
                   <div key={log.id} style={{display:'flex', gap:'10px', padding:'10px 12px', borderBottom:'1px dashed var(--border)', hover:{background:'var(--bg)'}}}>
                     <div style={{width:'36px', height:'36px', borderRadius:'4px', overflow:'hidden', flexShrink:0, border:'1px solid var(--border)'}}>
                        {log.snapshot_url ? (
                          <img src={`${API_BASE}${log.snapshot_url}`} alt="snapshot" style={{width:'100%', height:'100%', objectFit:'cover'}} />
                        ) : log.snapshot_path ? (
                          <img src={`${API_BASE}/api/snapshots/files/${log.snapshot_path.split('/').pop()}`} alt="snapshot" style={{width:'100%', height:'100%', objectFit:'cover'}} />
                        ) : (
                          <div style={{width:'100%', height:'100%', background:'#222', display:'flex', alignItems:'center', justifyContent:'center'}}>
                            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="var(--text-dim)"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>
                          </div>
                        )}
                     </div>
                     <div style={{flex:1, minWidth:0}}>
                        <div style={{fontSize:'12px', color:'var(--text-primary)', fontWeight:600, whiteSpace:'nowrap', overflow:'hidden', textOverflow:'ellipsis'}}>
                          {pName}
                        </div>
                        <div style={{display:'flex', justifyContent:'space-between', marginTop:'4px'}}>
                           <span style={{fontSize:'10px', color:'var(--accent)'}}>{similarity}</span>
                           <span style={{fontSize:'9px', fontFamily:"'Share Tech Mono',monospace", color:'var(--text-secondary)'}}>{formatTime(log.timestamp)}</span>
                        </div>
                     </div>
                   </div>
                 );
               })}
               {recognitionLogs.length === 0 && (
                 <div style={{fontSize: 10, color: 'var(--text-dim)', textAlign: 'center', padding: 20}}>
                    Đang chờ nhận diện...<br/>(AI Event Logging Active)
                 </div>
               )}
            </div>
          </div>
          
        </div>
      </div>
    </div>
  );
};

export default FaceIdView;
