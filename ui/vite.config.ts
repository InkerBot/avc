import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  // Absolute asset paths. The engine always serves the editor from the root,
  // and relative ones would break the moment an unknown path fell back to the
  // app shell: the shell's './assets/...' would resolve against that path.
  base: '/',
  build: { outDir: 'dist', emptyOutDir: true },
  server: {
    // In dev the page comes from vite and the data from the engine. Proxying
    // keeps them the same origin, so there is no CORS to open up.
    proxy: {
      '/api': { target: 'http://127.0.0.1:7420', changeOrigin: true },
    },
  },
})
