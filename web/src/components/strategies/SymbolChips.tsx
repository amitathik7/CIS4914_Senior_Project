import { symbolEntries } from "../../strategies/draft";

/** The symbols in the chosen dataset as toggles for the allowlist. Only a suggestion: the engine decides what a symbol list means. */
export function SymbolChips({ symbols, value, onChange }: { symbols: readonly string[]; value: string; onChange: (text: string) => void }) {
  if (symbols.length === 0) return null;
  const have = symbolEntries(value);
  const toggle = (s: string) => onChange((have.includes(s) ? have.filter((x) => x !== s) : [...have, s]).join(","));
  return (
    <div className="chips lab-chips" role="group" aria-label="Symbols in this dataset">
      <span className="faint">In this dataset:</span>
      {symbols.map((s) => <button key={s} type="button" className="chip-toggle" aria-pressed={have.includes(s)} onClick={() => toggle(s)}>{s}</button>)}
    </div>
  );
}
