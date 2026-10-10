import type { StrategySpec } from "../../strategies/catalog";

/** The engine's strategies as a radio group. Used by the Configure panel and by each side of Compare. */
export function StrategyKinds({ kinds, value, onSelect, label }: { kinds: readonly StrategySpec[]; value: string; onSelect: (kind: string) => void; label: string }) {
  return (
    <div className="kind-list" role="radiogroup" aria-label={label}>
      {kinds.map((k) => (
        <button
          key={k.kind} type="button" role="radio" aria-checked={k.kind === value} aria-pressed={k.kind === value}
          className="kind-option" disabled={!k.available} title={k.available ? k.kind : k.unavailableReason} onClick={() => onSelect(k.kind)}
        >
          <strong>{k.title}</strong>
          <small>{k.available ? k.kind : "Not available: the custom ML strategy has not been implemented"}</small>
        </button>
      ))}
      {kinds.length === 0 && <p className="faint">The engine's strategies appear here once the service is running.</p>}
    </div>
  );
}
