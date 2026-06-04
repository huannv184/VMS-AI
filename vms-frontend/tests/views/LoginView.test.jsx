import { describe, it, expect, beforeEach, vi } from 'vitest';
import React from 'react';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';

vi.mock('../../src/api/apiClient.js', () => ({
  default: {
    login: vi.fn(),
    verify2FA: vi.fn(),
    changePasswordOnLogin: vi.fn(),
  },
}));

import LoginView from '../../src/views/LoginView.jsx';
import apiClient from '../../src/api/apiClient.js';

beforeEach(() => {
  apiClient.login.mockReset();
});

describe('LoginView', () => {
  it('renders username + password inputs without crashing', () => {
    render(<LoginView onLoginSuccess={() => {}} />);
    expect(screen.getByPlaceholderText(/Tên đăng nhập|username/i)).toBeInTheDocument();
    expect(screen.getByPlaceholderText(/Mật khẩu|password/i)).toBeInTheDocument();
  });

  it('successful login → onLoginSuccess called', async () => {
    const onLoginSuccess = vi.fn();
    apiClient.login.mockResolvedValue({
      success: true,
      data: { token: 'jwt-ok' },
    });

    render(<LoginView onLoginSuccess={onLoginSuccess} />);
    const user = userEvent.setup();

    await user.type(screen.getByPlaceholderText(/Tên đăng nhập|username/i), 'admin');
    await user.type(screen.getByPlaceholderText(/Mật khẩu|password/i), 'secret123');
    await user.click(screen.getByRole('button', { name: /access system|đăng nhập|sign in|log in/i }));

    await waitFor(() => expect(onLoginSuccess).toHaveBeenCalledTimes(1));
    expect(apiClient.login).toHaveBeenCalledWith('admin', 'secret123');
  });

  it('failed login surfaces error message visibly (not silent)', async () => {
    apiClient.login.mockResolvedValue({
      success: false,
      data: null,
      error: 'Tài khoản hoặc mật khẩu không chính xác.',
    });

    render(<LoginView onLoginSuccess={() => {}} />);
    const user = userEvent.setup();

    await user.type(screen.getByPlaceholderText(/Tên đăng nhập|username/i), 'admin');
    await user.type(screen.getByPlaceholderText(/Mật khẩu|password/i), 'wrong');
    await user.click(screen.getByRole('button', { name: /access system|đăng nhập|sign in|log in/i }));

    await waitFor(() => {
      expect(screen.getByText(/Tài khoản hoặc mật khẩu không chính xác/i)).toBeInTheDocument();
    });
  });

  it('network error surfaces backend-unreachable message', async () => {
    apiClient.login.mockRejectedValue(new Error('connection refused'));

    render(<LoginView onLoginSuccess={() => {}} />);
    const user = userEvent.setup();

    await user.type(screen.getByPlaceholderText(/Tên đăng nhập|username/i), 'admin');
    await user.type(screen.getByPlaceholderText(/Mật khẩu|password/i), 'pwd');
    await user.click(screen.getByRole('button', { name: /access system|đăng nhập|sign in|log in/i }));

    await waitFor(() => {
      expect(screen.getByText(/Không thể kết nối tới máy chủ Backend/i)).toBeInTheDocument();
    });
  });
});
