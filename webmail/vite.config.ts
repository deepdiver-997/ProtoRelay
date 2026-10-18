import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
export default defineConfig(({ command }) => ({
  plugins: [
    react(),
    {
      name: "development-csp",
      // Vite injects inline refresh code and a WebSocket client in development.
      // The production document retains its restrictive CSP.
      transformIndexHtml:
        command === "serve"
          ? (html) =>
              html.replace(
                /\s*<meta\s+http-equiv="Content-Security-Policy"[\s\S]*?\/>/,
                "",
              )
          : undefined,
    },
  ],
  build: { outDir: "../build-webmail", emptyOutDir: true },
  server: {
    proxy: {
      "/api": {
        target: process.env.WEBMAIL_API_URL || "http://127.0.0.1:8081",
        changeOrigin: false,
      },
    },
  },
}));
