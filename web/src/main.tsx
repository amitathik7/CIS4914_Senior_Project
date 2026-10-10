import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { createBrowserRouter, Navigate, RouterProvider } from "react-router";
import { Shell } from "./components/Shell";
import { Backtests } from "./pages/Backtests";
import { NewBacktest } from "./pages/NewBacktest";
import { Orders } from "./pages/Orders";
import { Overview } from "./pages/Overview";
import { Report } from "./pages/Report";
import { Risk } from "./pages/Risk";
import { Strategies } from "./pages/Strategies";
import { StrategiesAdvanced } from "./pages/StrategiesAdvanced";
import { StrategiesCompare } from "./pages/StrategiesCompare";
import { StrategiesConfigure } from "./pages/StrategiesConfigure";
import { StrategiesExplore } from "./pages/StrategiesExplore";
import { System } from "./pages/System";
import { EngineProvider, useEngine } from "./state/engine";
import "./styles/global.css";
import "./styles/strategies.css";
import "./styles/compare.css";

function Home() {
  const { defaultRunId, runs } = useEngine();
  if (defaultRunId) return <Navigate to={`/runs/${defaultRunId}/overview`} replace />;
  if (runs && runs.length === 0) return <Navigate to="/backtests" replace />;
  return null;
}

// Strategies has three views (Configure, Explore, Compare) and an Advanced area for validation. Each keeps its state in a store that outlives the page, so it works with or without a selected run.
const strategiesRoute = (path: string) => ({
  path,
  element: <Strategies />,
  children: [
    { index: true, element: <Navigate to="configure" replace /> },
    { path: "configure", element: <StrategiesConfigure /> },
    { path: "explore", element: <StrategiesExplore /> },
    { path: "compare", element: <StrategiesCompare /> },
    { path: "advanced", element: <StrategiesAdvanced /> },
  ],
});

const router = createBrowserRouter([
  {
    element: <Shell />,
    children: [
      { index: true, element: <Home /> },
      { path: "runs/:runId", element: <Navigate to="overview" replace /> },
      { path: "runs/:runId/overview", element: <Overview /> },
      strategiesRoute("runs/:runId/strategies"),
      strategiesRoute("strategies"),
      { path: "runs/:runId/orders", element: <Orders /> },
      { path: "runs/:runId/risk", element: <Risk /> },
      { path: "runs/:runId/report", element: <Report /> },
      { path: "runs/:runId/system", element: <System /> },
      { path: "backtests", element: <Backtests /> },
      { path: "backtests/new", element: <NewBacktest /> },
      { path: "*", element: <Navigate to="/" replace /> },
    ],
  },
]);

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <EngineProvider>
      <RouterProvider router={router} />
    </EngineProvider>
  </StrictMode>,
);
