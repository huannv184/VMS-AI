import React, { useState, useEffect } from 'react';
import apiClient from '../api/apiClient';

const TwoFactorSetupModal = ({ onClose, onEnabled }) => {
  const [step, setStep] = useState(1); // 1: QR, 2: Verify
  const [setupData, setSetupData] = useState(null);
  const [code, setCode] = useState('');
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    fetchSetup();
  }, []);

  const fetchSetup = async () => {
    setLoading(true);
    const res = await apiClient.setup2FA();
    if (res.success && res.data?.secret) {
      setSetupData(res.data);
    } else {
      setError(res.error || 'Không thể lấy thông tin thiết lập 2FA');
    }
    setLoading(false);
  };

  const handleVerify = async (e) => {
    e.preventDefault();
    if (code.length !== 6) return;
    
    setLoading(true);
    setError('');
    const res = await apiClient.enable2FA(code, setupData?.secret);
    if (res.success) {
      onEnabled();
    } else {
      setError(res.error || 'Mã xác thực không hợp lệ. Vui lòng thử lại.');
    }
    setLoading(false);
  };

  // Backend may return a QR image (data URL or path). Fall back to quickchart.io —
  // chart.googleapis.com QR endpoint was retired in 2024 and returns 404.
  const backendQr = setupData?.qr_code || setupData?.qr_image || null;
  const qrUrl = backendQr
    ? backendQr
    : (setupData?.otpauth_url
        ? `https://quickchart.io/qr?text=${encodeURIComponent(setupData.otpauth_url)}&size=200`
        : '');

  const copySecret = async () => {
    if (!setupData?.secret) return;
    try {
      await navigator.clipboard.writeText(setupData.secret);
      const orig = error;
      setError('Đã sao chép Secret Key vào clipboard');
      setTimeout(() => setError(orig), 1500);
    } catch {
      setError('Không thể truy cập clipboard');
    }
  };

  return (
    <div className="modal-overlay" style={{
      position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.85)',
      display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 10000,
      backdropFilter: 'blur(10px)'
    }}>
      <div className="glass" style={{
        width: '400px', padding: '30px', borderRadius: '12px', border: '1px solid var(--border)',
        boxShadow: '0 20px 50px rgba(0,0,0,0.5)', textAlign: 'center'
      }}>
        <div style={{
          fontFamily: "'Rajdhani', sans-serif", fontSize: '24px', fontWeight: 700,
          color: 'var(--accent)', marginBottom: '10px', textTransform: 'uppercase', letterSpacing: '1px'
        }}>
          💡 Thiết Lập Bảo Mật 2 Lớp (2FA)
        </div>

        {loading && !setupData ? (
          <div className="ai-dot-spin" style={{ margin: '40px auto' }}></div>
        ) : (
          <>
            <div style={{ fontSize: '13px', color: 'var(--text-secondary)', marginBottom: '20px', lineHeight: 1.5 }}>
              Sử dụng ứng dụng xác thực (Google Authenticator, Authy...) để quét mã QR bên dưới.
            </div>

            {setupData && qrUrl && (
              <div style={{ background: '#fff', padding: '10px', borderRadius: '8px', display: 'inline-block', marginBottom: '20px' }}>
                <img
                  src={qrUrl}
                  alt="QR Code"
                  style={{ width: '180px', height: '180px' }}
                  onError={(e) => { e.currentTarget.style.display = 'none'; }}
                />
              </div>
            )}

            <div style={{ fontSize: '11px', color: 'var(--text-dim)', marginBottom: '8px', fontFamily: "'Share Tech Mono', monospace", wordBreak: 'break-all' }}>
              Secret Key: {setupData?.secret}
            </div>
            {setupData?.secret && (
              <div
                onClick={copySecret}
                style={{ fontSize: '10px', color: 'var(--accent)', cursor: 'pointer', marginBottom: '20px', textDecoration: 'underline' }}
              >
                Nhấn để sao chép Secret Key (nhập thủ công nếu QR không quét được)
              </div>
            )}

            <form onSubmit={handleVerify}>
              <div style={{ marginBottom: '15px' }}>
                <input
                  type="text"
                  placeholder="Nhập mã 6 chữ số"
                  value={code}
                  onChange={(e) => setCode(e.target.value.replace(/\D/g, '').slice(0, 6))}
                  className="config-input"
                  style={{
                    width: '100%', textAlign: 'center', fontSize: '24px', letterSpacing: '4px',
                    fontFamily: "'Share Tech Mono', monospace", background: 'rgba(255,255,255,0.05)'
                  }}
                  autoFocus
                />
              </div>

              {error && <div style={{ color: 'var(--danger)', fontSize: '12px', marginBottom: '15px' }}>{error}</div>}

              <div style={{ display: 'flex', gap: '10px' }}>
                <button type="button" onClick={onClose} className="config-btn" style={{ flex: 1 }}>HỦY</button>
                <button
                  type="submit"
                  disabled={code.length !== 6 || loading}
                  className="pb-btn"
                  style={{ flex: 2, background: 'var(--accent)', color: '#000', fontWeight: 700 }}
                >
                  {loading ? 'ĐANG XỬ LÝ...' : 'XÁC THỰC & KÍCH HOẠT'}
                </button>
              </div>
            </form>
          </>
        )}
      </div>
    </div>
  );
};

export default TwoFactorSetupModal;
