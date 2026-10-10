import { DIFF_MODE_TEXT, nextDifference, nextSignalEither, type CompareModel, type DiffMode } from "../../../strategies/compare";
import { nextEvent, previousEvent, selectedEvent } from "../../../strategies/cursor";
import { IconNextSignal, IconReset, IconStepBack, IconStepNext } from "../../icons";

type Step = "next" | "previous" | "next_signal" | "next_difference" | "reset";

/** One position for both configurations. Moving it only reveals rows of results that already exist; it never reruns either strategy. */
export function CompareControls({ model, cursor, symbol, mode, onStep, onCursor, onMode }: {
  model: CompareModel;
  cursor: number;
  symbol: string | null;
  mode: DiffMode;
  onStep: (direction: Step) => void;
  onCursor: (position: number) => void;
  onMode: (mode: DiffMode) => void;
}) {
  const side = model.members.A; // both sides hold the same rows in the same order
  const hasPrevious = previousEvent(side, cursor, symbol) !== undefined;
  const hasNext = nextEvent(side, cursor, symbol) !== undefined;
  const hasSignal = nextSignalEither(model, cursor, symbol) !== undefined;
  const hasDifference = nextDifference(model, cursor, symbol, mode) !== undefined;
  const selected = selectedEvent(side, cursor);
  const where = symbol === null ? "any symbol" : symbol;
  return (
    <div className="lab-controls">
      <div className="lab-buttons" role="group" aria-label="Replay controls">
        <button type="button" className="btn btn-sm" onClick={() => onStep("reset")} disabled={cursor === 0} title="Go back to position 0: nothing revealed, for both configurations">
          <IconReset size={13} /> Reset
        </button>
        <button type="button" className="btn btn-sm" onClick={() => onStep("previous")} disabled={!hasPrevious} title={`Step back to the previous recorded row (${where})`}>
          <IconStepBack size={13} /> Previous
        </button>
        <button type="button" className="btn btn-sm" onClick={() => onStep("next")} disabled={!hasNext} title={`Reveal the next recorded row (${where})`}>
          Next event <IconStepNext size={13} />
        </button>
        <button type="button" className="btn btn-sm" onClick={() => onStep("next_signal")} disabled={!hasSignal} title={`Jump to the next row where A or B made a Buy or Sell request (${where})`}>
          Next signal <IconNextSignal size={13} />
        </button>
        <button type="button" className="btn btn-sm" onClick={() => onStep("next_difference")} disabled={!hasDifference} title={`Jump to the next row where the two configurations differ: ${DIFF_MODE_TEXT[mode].toLowerCase()} (${where})`}>
          Next difference <IconNextSignal size={13} />
        </button>
        <label className="row cmp-mode" title="Which rows 'Next difference' stops at. Rows one side did not evaluate are included only in the second option.">
          <span className="muted">A difference is</span>
          <select className="select" style={{ width: 210 }} value={mode} aria-label="What counts as a difference" onChange={(e) => onMode(e.target.value as DiffMode)}>
            {(Object.keys(DIFF_MODE_TEXT) as DiffMode[]).map((m) => <option key={m} value={m}>{DIFF_MODE_TEXT[m]}</option>)}
          </select>
        </label>
      </div>
      <div className="lab-slider-row">
        <label htmlFor="cmp-position" className="muted">Replay position</label>
        <input
          id="cmp-position" className="lab-slider" type="range" min={0} max={model.total} step={1} value={cursor}
          aria-valuetext={cursor === 0 ? `Nothing revealed of ${model.total} events` : `Event ${cursor} of ${model.total}`}
          onChange={(e) => onCursor(Number(e.target.value))}
        />
        <span className="num lab-position-text">
          {cursor === 0 ? `0 of ${model.total}` : `${cursor} of ${model.total}`}
          <span className="faint">{selected ? ` · ${selected.symbol}` : " · nothing revealed"}</span>
        </span>
      </div>
    </div>
  );
}
