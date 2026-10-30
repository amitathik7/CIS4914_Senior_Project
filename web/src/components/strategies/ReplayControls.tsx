import { nextEvent, nextSignal, previousEvent, selectedEvent, type ReplayModel } from "../../strategies/cursor";
import { IconNextSignal, IconReset, IconStepBack, IconStepNext } from "../icons";

export function ReplayControls({ model, cursor, symbol, onStep, onCursor }: {
  model: ReplayModel;
  cursor: number;
  symbol: string | null;
  onStep: (direction: "next" | "previous" | "next_signal" | "reset") => void;
  onCursor: (position: number) => void;
}) {
  const hasPrevious = previousEvent(model, cursor, symbol) !== undefined;
  const hasNext = nextEvent(model, cursor, symbol) !== undefined;
  const hasSignal = nextSignal(model, cursor, symbol) !== undefined;
  const selected = selectedEvent(model, cursor);
  const where = symbol === null ? "any symbol" : symbol;
  return (
    <div className="lab-controls">
      <div className="lab-buttons" role="group" aria-label="Replay controls">
        <button type="button" className="btn btn-sm" onClick={() => onStep("reset")} disabled={cursor === 0} title="Go back to position 0: nothing revealed">
          <IconReset size={13} /> Reset
        </button>
        <button type="button" className="btn btn-sm" onClick={() => onStep("previous")} disabled={!hasPrevious} title={`Step back to the previous recorded row (${where})`}>
          <IconStepBack size={13} /> Previous
        </button>
        <button type="button" className="btn btn-sm" onClick={() => onStep("next")} disabled={!hasNext} title={`Reveal the next recorded row (${where})`}>
          Next event <IconStepNext size={13} />
        </button>
        <button type="button" className="btn btn-sm" onClick={() => onStep("next_signal")} disabled={!hasSignal} title={`Jump to the next row that produced a Buy or Sell request (${where})`}>
          Next signal <IconNextSignal size={13} />
        </button>
      </div>
      <div className="lab-slider-row">
        <label htmlFor="lab-position" className="muted">Replay position</label>
        <input
          id="lab-position" className="lab-slider" type="range" min={0} max={model.total} step={1} value={cursor}
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
