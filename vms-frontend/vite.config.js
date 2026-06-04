import { defineConfig, loadEnv } from 'vite'
import react from '@vitejs/plugin-react'

const trimTrailingSlash = (value = '') => value.replace(/\/+$/, '')

const ensureHttpTarget = (value, fallback) => {
  const raw = trimTrailingSlash(value || fallback)
  try {
    const url = new URL(raw)
    if (url.protocol === 'ws:') url.protocol = 'http:'
    if (url.protocol === 'wss:') url.protocol = 'https:'
    return trimTrailingSlash(url.toString())
  } catch {
    return fallback
  }
}

const createProxyErrorHandler = (contentType, bodyFactory) => (proxy) => {
  proxy.on('error', (err, _req, res) => {
    if (err.code === 'ECONNRESET' || err.code === 'ECONNREFUSED' || err.code === 'EPIPE') {
      if (res && typeof res.writeHead === 'function' && !res.headersSent) {
        res.writeHead(502, { 'Content-Type': contentType })
        res.end(bodyFactory())
      }
      return
    }
  })
}

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), '')
  // Dev default: when VITE_API_URL is unset (the documented same-origin
  // dev shape), still register the /api proxy pointing at the local backend.
  // Pre-fix the fallback was '' which left `if (apiTarget)` false and the
  // proxy unregistered, so /api/* fell into the SPA catch-all and FE saw
  // index.html instead of backend JSON.
  const apiTarget = ensureHttpTarget(env.VITE_API_URL, 'http://localhost:8000')
  // Build WS proxy target: strip the /ws path suffix since Vite proxy
  // already maps the '/ws' mount-point and appends it to the target.
  const wsRawTarget = ensureHttpTarget(env.VITE_WS_URL || env.VITE_API_URL, '')
  const wsTarget = (() => {
    if (!wsRawTarget) return ''
    try {
      const u = new URL(wsRawTarget)
      u.pathname = '/'
      u.search = ''
      u.hash = ''
      return trimTrailingSlash(u.toString())
    } catch {
      return wsRawTarget
    }
  })()
  const proxy = {}

  if (wsTarget) {
    proxy['/ws'] = {
      target: wsTarget,
      changeOrigin: true,
      ws: true,
      configure: (proxyInstance) => {
        createProxyErrorHandler('text/plain', () => 'WebSocket backend unavailable')(proxyInstance)
        proxyInstance.on('proxyReqWs', (_proxyReq, _req, socket) => {
          socket.on('error', (err) => {
            if (err.code === 'ECONNRESET' || err.code === 'EPIPE') return
          })
        })
      },
    }
  }

  if (apiTarget) {
    proxy['/api'] = {
      target: apiTarget,
      changeOrigin: true,
      secure: false,
      timeout: 0,
      configure: createProxyErrorHandler(
        'application/json',
        () => JSON.stringify({ error: 'Backend unavailable' }),
      ),
    }

    proxy['/snapshots'] = {
      target: apiTarget,
      changeOrigin: true,
      secure: false,
    }

    proxy['/recordings'] = {
      target: apiTarget,
      changeOrigin: true,
      secure: false,
    }
  }

  return {
    plugins: [react()],
    build: {
      outDir: 'dist',
      emptyOutDir: true,
    },
    server: {
      port: 3000,
      strictPort: true,
      proxy,
    },
    test: {
      environment: 'jsdom',
      globals: true,
      setupFiles: ['./tests/setup.js'],
      include: ['tests/**/*.test.{js,jsx}'],
      css: false,
    },
  }
})
