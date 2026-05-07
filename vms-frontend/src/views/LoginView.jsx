import React, { useState } from 'react';
import { User, Lock, Shield, Eye, EyeOff, LogIn } from 'lucide-react';
import apiClient from '../api/apiClient';

const LoginView = ({ onLoginSuccess }) => {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [showPassword, setShowPassword] = useState(false);
  // 1: Login, 2: 2FA, 3: Mandatory password change (SEC-001)
  const [step, setStep] = useState(1);
  const [twoFactorCode, setTwoFactorCode] = useState('');
  const [tempToken, setTempToken] = useState('');
  const [newPassword, setNewPassword] = useState('');
  const [confirmPassword, setConfirmPassword] = useState('');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');

  // SEC-001: backend may respond with requires_password_change=true and a
  // password_change_pending temp_token in lieu of an access token. The UI
  // must route the user to step 3 — the previous version ignored the flag
  // and treated `data.token == null` as a generic "wrong credentials" error,
  // which left default-password admins stranded with no recovery.
  const routePostAuthResult = (result) => {
    if (!result?.success) {
      setError(result?.error || 'Tài khoản hoặc mật khẩu không chính xác.');
      return false;
    }
    if (result.data?.requires_2fa) {
      setTempToken(result.data.temp_token || '');
      setStep(2);
      return true;
    }
    if (result.data?.password_change_required) {
      setTempToken(result.data.temp_token || '');
      setNewPassword('');
      setConfirmPassword('');
      setStep(3);
      return true;
    }
    if (result.data?.token) {
      onLoginSuccess();
      return true;
    }
    setError('Tài khoản hoặc mật khẩu không chính xác.');
    return false;
  };

  const handleSubmit = async (e) => {
    e.preventDefault();
    setLoading(true);
    setError('');

    try {
      const result = await apiClient.login(username, password);
      routePostAuthResult(result);
    } catch {
      setError('Không thể kết nối tới máy chủ Backend.');
    } finally {
      setLoading(false);
    }
  };

  const handle2FASubmit = async (e) => {
    e.preventDefault();
    if (twoFactorCode.length !== 6) return;

    setLoading(true);
    setError('');
    try {
      const result = await apiClient.verify2FA(tempToken, twoFactorCode);
      // 2FA verify can also flip into the password-change gate when the
      // admin used 2FA but is still on the factory-default password.
      routePostAuthResult(result);
    } catch {
      setError('Lỗi xác thực 2FA.');
    } finally {
      setLoading(false);
    }
  };

  const handlePasswordChangeSubmit = async (e) => {
    e.preventDefault();
    if (newPassword.length < 8) {
      setError('Mật khẩu mới phải có ít nhất 8 ký tự.');
      return;
    }
    if (newPassword !== confirmPassword) {
      setError('Mật khẩu xác nhận không khớp.');
      return;
    }
    if (newPassword === password) {
      setError('Mật khẩu mới không được trùng mật khẩu cũ.');
      return;
    }

    setLoading(true);
    setError('');
    try {
      const result = await apiClient.changePasswordOnLogin(tempToken, newPassword);
      if (result.success && result.data?.token) {
        onLoginSuccess();
      } else {
        setError(result.error || 'Không đổi được mật khẩu. Vui lòng đăng nhập lại.');
      }
    } catch {
      setError('Lỗi đổi mật khẩu.');
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="login-container">
      <div className="login-glow"></div>
      
      <div className="login-card glass">
        <div className="login-header">
           <div className="login-logo">
             <Shield size={32} color="var(--accent)" />
           </div>
           <h1>VMS COMMAND CENTER</h1>
           <p>SYSTEM ACCESS REQUIRED</p>
        </div>

        {step === 1 && (
          <form onSubmit={handleSubmit} className="login-form">
            <div className="login-input-group">
              <User size={16} className="input-icon" />
              <input
                type="text"
                placeholder="USERNAME"
                value={username}
                onChange={(e) => setUsername(e.target.value)}
                required
              />
            </div>

            <div className="login-input-group">
              <Lock size={16} className="input-icon" />
              <input
                type={showPassword ? "text" : "password"}
                placeholder="PASSWORD"
                value={password}
                onChange={(e) => setPassword(e.target.value)}
                required
              />
              <div className="pwd-toggle" onClick={() => setShowPassword(!showPassword)}>
                {showPassword ? <EyeOff size={16} /> : <Eye size={16} />}
              </div>
            </div>

            {error && <div className="login-error">{error}</div>}

            <button
              type="submit"
              className={`login-btn ${loading ? 'loading' : ''}`}
              disabled={loading}
            >
              {loading ? (
                <div className="spinner-small"></div>
              ) : (
                <>
                  <LogIn size={18} /> ACCESS SYSTEM
                </>
              )}
            </button>
          </form>
        )}

        {step === 2 && (
          <form onSubmit={handle2FASubmit} className="login-form">
            <div style={{ textAlign: 'center', marginBottom: '10px' }}>
               <Shield size={24} color="var(--accent3)" />
               <div style={{ fontSize: '12px', fontWeight: 700, color: 'var(--accent3)', marginTop: '5px' }}>TWO-FACTOR AUTHENTICATION</div>
               <div style={{ fontSize: '10px', color: 'rgba(255,255,255,0.4)', marginTop: '3px' }}>Enter the 6-digit code from your app</div>
            </div>

            <div className="login-input-group">
              <Lock size={16} className="input-icon" />
              <input
                type="text"
                placeholder="000000"
                value={twoFactorCode}
                onChange={(e) => setTwoFactorCode(e.target.value.replace(/\D/g, '').slice(0, 6))}
                required
                autoFocus
                style={{ textAlign: 'center', letterSpacing: '4px', fontSize: '18px' }}
              />
            </div>

            {error && <div className="login-error">{error}</div>}

            <button
              type="submit"
              className={`login-btn ${loading ? 'loading' : ''}`}
              disabled={loading || twoFactorCode.length !== 6}
              style={{ background: 'var(--accent3)' }}
            >
              {loading ? <div className="spinner-small"></div> : 'VERIFY & LOGIN'}
            </button>
            <div style={{ textAlign: 'center', marginTop: '10px' }}>
              <span
                style={{ fontSize: '11px', color: 'rgba(255,255,255,0.4)', cursor: 'pointer', textDecoration: 'underline' }}
                onClick={() => setStep(1)}
              >
                Back to Login
              </span>
            </div>
          </form>
        )}

        {step === 3 && (
          <form onSubmit={handlePasswordChangeSubmit} className="login-form">
            <div style={{ textAlign: 'center', marginBottom: '10px' }}>
               <Shield size={24} color="var(--danger)" />
               <div style={{ fontSize: '12px', fontWeight: 700, color: 'var(--danger)', marginTop: '5px' }}>PASSWORD CHANGE REQUIRED</div>
               <div style={{ fontSize: '10px', color: 'rgba(255,255,255,0.5)', marginTop: '3px' }}>
                 Default credentials detected. Set a new password to continue.
               </div>
            </div>

            <div className="login-input-group">
              <Lock size={16} className="input-icon" />
              <input
                type={showPassword ? 'text' : 'password'}
                placeholder="NEW PASSWORD (min 8 chars)"
                value={newPassword}
                onChange={(e) => setNewPassword(e.target.value)}
                required
                minLength={8}
                maxLength={256}
                autoFocus
              />
              <div className="pwd-toggle" onClick={() => setShowPassword(!showPassword)}>
                {showPassword ? <EyeOff size={16} /> : <Eye size={16} />}
              </div>
            </div>

            <div className="login-input-group">
              <Lock size={16} className="input-icon" />
              <input
                type={showPassword ? 'text' : 'password'}
                placeholder="CONFIRM NEW PASSWORD"
                value={confirmPassword}
                onChange={(e) => setConfirmPassword(e.target.value)}
                required
                minLength={8}
                maxLength={256}
              />
            </div>

            {error && <div className="login-error">{error}</div>}

            <button
              type="submit"
              className={`login-btn ${loading ? 'loading' : ''}`}
              disabled={loading || newPassword.length < 8 || confirmPassword.length < 8}
            >
              {loading ? <div className="spinner-small"></div> : 'SET NEW PASSWORD & ACCESS'}
            </button>
            <div style={{ textAlign: 'center', marginTop: '10px' }}>
              <span
                style={{ fontSize: '11px', color: 'rgba(255,255,255,0.4)', cursor: 'pointer', textDecoration: 'underline' }}
                onClick={() => { setStep(1); setError(''); setTempToken(''); }}
              >
                Cancel
              </span>
            </div>
          </form>
        )}

        <div className="login-footer">
          <span>SECURE CHANNEL ENCRYPTED</span>
          <div className="status-dot online"></div>
        </div>
      </div>

      <style dangerouslySetInnerHTML={{ __html: `
        .login-container {
          position: fixed;
          inset: 0;
          display: flex;
          align-items: center;
          justify-content: center;
          background: #000810;
          z-index: 10000;
          font-family: 'Rajdhani', sans-serif;
        }

        .login-glow {
          position: absolute;
          width: 600px;
          height: 600px;
          background: radial-gradient(circle, rgba(0, 203, 255, 0.1) 0%, transparent 70%);
          pointer-events: none;
        }

        .login-card {
          width: 380px;
          padding: 40px;
          border-radius: 4px;
          border: 1px solid rgba(0, 203, 255, 0.2);
          position: relative;
          background: rgba(10, 20, 30, 0.8);
          box-shadow: 0 0 50px rgba(0,0,0,0.5);
        }

        .login-header {
          text-align: center;
          margin-bottom: 35px;
        }

        .login-logo {
          margin-bottom: 15px;
          display: inline-block;
          filter: drop-shadow(0 0 10px var(--accent));
        }

        .login-header h1 {
          font-size: 20px;
          letter-spacing: 4px;
          color: white;
          margin: 0;
        }

        .login-header p {
          font-size: 10px;
          letter-spacing: 2px;
          color: var(--accent);
          margin: 10px 0 0;
          opacity: 0.8;
        }

        .login-form {
          display: flex;
          flex-direction: column;
          gap: 20px;
        }

        .login-input-group {
          position: relative;
          background: rgba(0,0,0,0.3);
          border: 1px solid rgba(255,255,255,0.1);
          border-radius: 4px;
          transition: all 0.3s;
        }

        .login-input-group:focus-within {
          border-color: var(--accent);
          box-shadow: 0 0 10px rgba(0, 203, 255, 0.2);
        }

        .input-icon {
          position: absolute;
          left: 12px;
          top: 50%;
          transform: translateY(-50%);
          color: rgba(255,255,255,0.4);
        }

        .login-input-group input {
          width: 100%;
          background: transparent;
          border: none;
          color: white;
          padding: 12px 40px;
          font-size: 13px;
          letter-spacing: 1px;
          outline: none;
        }

        .pwd-toggle {
          position: absolute;
          right: 12px;
          top: 50%;
          transform: translateY(-50%);
          color: rgba(255,255,255,0.4);
          cursor: pointer;
        }

        .login-error {
          color: var(--danger);
          font-size: 11px;
          text-align: center;
          background: rgba(139, 0, 0, 0.1);
          padding: 8px;
          border-radius: 2px;
          border-left: 2px solid var(--danger);
        }

        .login-btn {
          background: var(--accent);
          color: #000;
          border: none;
          padding: 14px;
          border-radius: 2px;
          font-family: inherit;
          font-weight: 700;
          font-size: 14px;
          letter-spacing: 2px;
          cursor: pointer;
          display: flex;
          align-items: center;
          justify-content: center;
          gap: 10px;
          transition: all 0.3s;
        }

        .login-btn:hover:not(:disabled) {
          background: white;
          transform: translateY(-1px);
          box-shadow: 0 5px 15px rgba(0, 203, 255, 0.4);
        }

        .login-btn:disabled {
          opacity: 0.5;
          cursor: wait;
        }

        .login-footer {
          margin-top: 30px;
          display: flex;
          align-items: center;
          justify-content: center;
          gap: 10px;
          font-size: 9px;
          color: rgba(255,255,255,0.3);
          letter-spacing: 1px;
        }

        .online {
          width: 6px;
          height: 6px;
          border-radius: 50%;
          background: var(--accent3);
          box-shadow: 0 0 5px var(--accent3);
        }

        .spinner-small {
          width: 16px;
          height: 16px;
          border: 2px solid rgba(0,0,0,0.1);
          border-top-color: black;
          border-radius: 50%;
          animation: spin 1s linear infinite;
        }

        @keyframes spin {
          to { transform: rotate(360deg); }
        }
      `}} />
    </div>
  );
};

export default LoginView;
