import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// Dev: `npm run dev` proxies API calls to a local server on :8080.
export default defineConfig({
  plugins: [vue()],
  server: {
    proxy: {
      '/api': 'http://127.0.0.1:8080',
      '/device': { target: 'http://127.0.0.1:8080', ws: true },
      '/web/ws': { target: 'http://127.0.0.1:8080', ws: true },
    },
  },
})
