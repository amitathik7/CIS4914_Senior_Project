import { Traceability } from "../components/strategies/Traceability";
import { ValidationPanel } from "../components/strategies/ValidationPanel";

/** Advanced: validation of the executable and the identities behind what the other views show. Deliberately not a top-level view. */
export function StrategiesAdvanced() {
  return (
    <div className="stack" style={{ gap: 14 }}>
      <ValidationPanel />
      <Traceability />
    </div>
  );
}
