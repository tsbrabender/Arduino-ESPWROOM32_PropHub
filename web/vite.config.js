import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  base: './',
  build: {
    outDir: '../data',
    emptyOutDir: true,
  },
  server: {
    proxy: {
      '/api': 'http://prophub.local',
    },
  },
})
