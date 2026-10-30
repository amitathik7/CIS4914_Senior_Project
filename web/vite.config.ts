/// <reference types="vitest/config" />
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// The Strategy Lab gateway (python/strategy_lab/lab_gateway.py) that runs the real C++ strategy engine for the Strategies
// section. It is separate from the engine API above: see docs/STRATEGIES_CONSOLE.md.
const labGateway = process.env.LAB_GATEWAY ?? "http://127.0.0.1:8765";
const proxy = {
  "/api": { target: "http://127.0.0.1:8080", changeOrigin: false },
  "/lab": { target: labGateway, changeOrigin: false },
};

export default defineConfig({
  plugins: [react()],
  server: {
    host: "127.0.0.1",
    port: 5173,
    // Engine API, when one exists (see docs/adr/0006-web-frontend-api-contract.md), and the Strategy Lab gateway.
    proxy,
  },
  preview: { host: "127.0.0.1", port: 4173, proxy },
  build: {
    rolldownOptions: {
      output: {
        codeSplitting: {
          groups: [
            { name: "charts", test: /node_modules[\\/]lightweight-charts/ },
            { name: "react", test: /node_modules[\\/](react|react-dom|react-router|scheduler)[\\/]/ },
          ],
        },
      },
    },
  },
  test: { environment: "node", include: ["src/**/*.test.ts"] },
});
