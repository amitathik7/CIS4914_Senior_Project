import { Outlet, useLocation, useNavigate, useParams } from "react-router";
import { ServiceBadge, ServiceNotice } from "../components/strategies/ServiceStatus";
import { Segmented } from "../components/ui";
import { useLab, useLabConnection } from "../strategies/useLab";

type View = "configure" | "explore" | "compare";

/** Strategies: the shared header (views, engine status) around Configure, Explore and Compare, and the Advanced area. Each view keeps its state in a store, so it survives switching views and runs. */
export function Strategies() {
  const { runId } = useParams();
  const location = useLocation();
  const navigate = useNavigate();
  const state = useLab();
  useLabConnection();

  const base = runId ? `/runs/${runId}/strategies` : "/strategies";
  const advanced = location.pathname.endsWith("/advanced");
  const view: View = location.pathname.endsWith("/explore") ? "explore" : location.pathname.endsWith("/compare") ? "compare" : "configure";

  return (
    <>
      <div className="page-head">
        <div>
          <h1>Strategies</h1>
          <p>Set up a strategy, replay recorded bars through the real engine, step through every decision it made, and compare two configurations side by side.</p>
        </div>
        <div className="page-actions">
          <ServiceBadge state={state} />
          <Segmented<View> label="Strategies view" value={advanced ? ("" as View) : view} onChange={(v) => navigate(`${base}/${v}`)} options={[
            { value: "configure", label: "Configure" },
            { value: "explore", label: "Explore" },
            { value: "compare", label: "Compare" },
          ]} />
          <button type="button" className="btn btn-ghost" aria-pressed={advanced} onClick={() => navigate(`${base}/${advanced ? "configure" : "advanced"}`)} title="Validation of the replay engine and the identities behind what these views show">
            Advanced
          </button>
        </div>
      </div>
      <div style={{ marginBottom: 14 }}><ServiceNotice state={state} /></div>
      <Outlet />
    </>
  );
}
