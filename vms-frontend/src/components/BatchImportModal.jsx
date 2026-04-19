import React, { useState, useRef } from 'react';
import apiClient from '../api/apiClient';

/**
 * BATCH IMPORT MODAL (Requirement 1.3: Import hàng loạt CSV)
 */
const BatchImportModal = ({ onClose, onImported }) => {
  const [csvText, setCsvText] = useState('');
  const [isImporting, setIsImporting] = useState(false);
  const [result, setResult] = useState(null);
  const fileInputRef = useRef(null);

  const handleFileUpload = (e) => {
    const file = e.target.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = (evt) => setCsvText(evt.target.result);
    reader.readAsText(file);
  };

  const handleImport = async () => {
    if (!csvText.trim()) return;
    setIsImporting(true);
    setResult(null);
    try {
      const res = await apiClient.importCameras(csvText, true);
      const payload = res.success ? (res.data || {}) : { error: res.error };
      setResult(payload);
      if (res.success && (payload.imported || 0) > 0) {
        if (onImported) onImported();
      }
    } catch (err) {
      setResult({ error: err.message });
    } finally {
      setIsImporting(false);
    }
  };

  const downloadTemplate = () => {
    const headers = "name,rtsp_url,sub_stream_url,description\nPhòng Họp 1,rtsp://admin:12345@192.168.1.10/stream1,rtsp://admin:12345@192.168.1.10/stream2,Tầng 1\nSảnh Chính,rtsp://admin:12345@192.168.1.11/stream1,,Gần cổng ra vào\n";
    const blob = new Blob(["\uFEFF" + headers], { type: 'text/csv;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'vms_camera_template.csv';
    a.click();
  };

  return (
    <div className="modal-overlay" style={{
      position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.85)', backdropFilter: 'blur(8px)',
      display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 10000, padding: '20px'
    }}>
      <div className="vms-modal" style={{
        maxWidth: '640px', width: '100%', background: '#0a0d12', border: '1px solid rgba(0,215,255,0.2)',
        borderRadius: '8px', boxShadow: '0 20px 40px rgba(0,0,0,0.6)', overflow: 'hidden', display: 'flex', flexDirection: 'column'
      }}>
        <div className="vms-modal-header" style={{
          padding: '16px 20px', borderBottom: '1px solid rgba(255,255,255,0.05)', display: 'flex',
          justifyContent: 'space-between', alignItems: 'center', background: 'rgba(255,255,255,0.02)'
        }}>
          <div style={{ fontSize: '16px', fontWeight: 'bold', color: 'var(--primary)', display: 'flex', alignItems: 'center' }}>
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" style={{width:'18px',height:'18px',marginRight:'10px'}}><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/></svg>
            Import Camera Hàng Loạt (CSV/JSON)
          </div>
          <div style={{ cursor: 'pointer', fontSize: '24px', opacity: 0.6 }} onClick={onClose}>&times;</div>
        </div>
        
        <div className="vms-modal-body" style={{ padding: '20px', maxHeight: '70vh', overflowY: 'auto' }}>
          <div style={{ marginBottom: '20px', padding: '12px', background: 'rgba(0,215,255,0.05)', borderRadius: '6px', fontSize: '13px', border: '1px solid rgba(0,215,255,0.1)' }}>
            <p style={{ margin: '0 0 8px 0' }}>💡 <b>Hướng dẫn:</b> Bạn có thể chọn file CSV hoặc dán nội dung thô vào ô dưới đây.</p>
            <div style={{ display: 'flex', gap: '10px' }}>
              <button className="config-btn" onClick={() => fileInputRef.current.click()} disabled={isImporting}>
                📂 Chọn File CSV
              </button>
              <button className="config-btn" style={{ background: 'transparent', border: '1px solid rgba(255,255,255,0.1)' }} onClick={downloadTemplate}>
                📝 Tải Mẫu CSV
              </button>
              <input type="file" ref={fileInputRef} onChange={handleFileUpload} style={{ display: 'none' }} accept=".csv" />
            </div>
          </div>

          <textarea
            style={{
              width: '100%', height: '240px', background: '#05070a', border: '1px solid rgba(255,255,255,0.1)',
              borderRadius: '4px', padding: '12px', color: '#fff', fontSize: '12px', fontFamily: 'monospace',
              resize: 'none', outline: 'none'
            }}
            placeholder="Dán nội dung CSV (ví dụ: name,rtsp_url...)..."
            value={csvText}
            onChange={(e) => setCsvText(e.target.value)}
            disabled={isImporting}
          />

          {result && (
            <div style={{ 
              marginTop: '15px', padding: '12px', background: result.error ? 'rgba(255,50,50,0.1)' : 'rgba(0,255,100,0.05)', 
              borderRadius: '6px', borderLeft: `4px solid ${result.error ? '#ff4d4d' : '#2ecc71'}` 
            }}>
              {result.error ? (
                <div style={{ color: '#ff4d4d', fontSize: '13px' }}>❌ Lỗi: {result.error}</div>
              ) : (
                <div style={{ fontSize: '13px' }}>
                  <div style={{ color: '#2ecc71', fontWeight: 'bold' }}>✅ Đã xử lý: {result.imported} camera</div>
                  {result.failed > 0 && <div style={{ color: '#ffb300' }}>⚠️ Thất bại: {result.failed}</div>}
                  {result.errors?.length > 0 && (
                    <div style={{ marginTop: '8px', fontSize: '11px', opacity: 0.8, background: 'rgba(0,0,0,0.2)', padding: '6px' }}>
                      {result.errors.slice(0, 5).map((e, idx) => (
                        <div key={idx} style={{ marginBottom: '2px' }}>• {e.name || 'Unknown'}: {e.error}</div>
                      ))}
                      {result.errors.length > 5 && <div>... và {result.errors.length - 5} lỗi khác</div>}
                    </div>
                  )}
                </div>
              )}
            </div>
          )}
        </div>

        <div className="vms-modal-footer" style={{ 
          padding: '16px 20px', borderTop: '1px solid rgba(255,255,255,0.05)', 
          display: 'flex', justifyContent: 'flex-end', gap: '12px', background: 'rgba(255,255,255,0.01)'
        }}>
          <button className="config-btn" style={{ background: 'rgba(255,255,255,0.05)' }} onClick={onClose} disabled={isImporting}>
            Đóng
          </button>
          <button 
            className={`config-btn ${(!csvText.trim() || isImporting) ? 'disabled' : ''}`}
            onClick={handleImport}
            disabled={!csvText.trim() || isImporting}
            style={{ minWidth: '140px', background: 'var(--primary)' }}
          >
            {isImporting ? '⏳ Đang xử lý...' : '📥 Import Ngay'}
          </button>
        </div>
      </div>
    </div>
  );
};

export default BatchImportModal;
