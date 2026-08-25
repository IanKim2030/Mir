import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// 빌드 산출물은 control 모듈 안으로 — go:embed 대상.
// base './' : 임베드 서버가 어느 경로에 있든 상대경로 자산이 로드된다.
export default defineConfig({
  plugins: [react()],
  base: './',
  build: {
    outDir: '../control/internal/webui/dist',
    emptyOutDir: true,
  },
  server: {
    // 로컬 개발: API 는 별도로 도는 mir-control(:8080)로 프록시.
    proxy: {
      '/api': 'http://localhost:8080',
      '/healthz': 'http://localhost:8080',
      '/readyz': 'http://localhost:8080',
    },
  },
})
