import { useNavigate, useParams } from "react-router";
import { AboutStrategy } from "../components/strategies/AboutStrategy";
import { ConfigPanel } from "../components/strategies/ConfigPanel";
import { RecordedSettings } from "../components/strategies/RecordedSettings";
import { labStore, useLab } from "../strategies/useLab";

export function StrategiesConfigure() {
  const state = useLab();
  const navigate = useNavigate();
  const { runId } = useParams();
  // Starting a replay is always explicit; from here it also opens Explore, where the result appears.
  const run = () => {
    navigate(`${runId ? `/runs/${runId}/strategies` : "/strategies"}/explore`);
    void labStore.runReplay();
  };
  return (
    <div className="lab">
      <aside className="lab-side"><ConfigPanel state={state} onRun={run} /></aside>
      <div className="lab-main">
        <AboutStrategy state={state} />
        <RecordedSettings state={state} />
      </div>
    </div>
  );
}
