import { symbolEntries } from "../../strategies/draft";

/** The symbols in the chosen dataset as toggles for the allowlist. Only a suggestion: the engine decides what a symbol list means. */
export function SymbolChips({ symbols, value, onChange }: { symbols: readonly string[]; value: string; onChange: (text: string) => void }) {
  if (symbols.length === 0) return null;
  const have = symbolEntries(value);
  // Symbols are matched exactly, so a typo or the wrong case silently matches nothing: say so before the run.
  const missing = have.filter((s) => !symbols.includes(s));
  const toggle = (s: string) => onChange((have.includes(s) ? have.filter((x) => x !== s) : [...have, s]).join(","));
  return (
    <div className="chips lab-chips" role="group" aria-label="Symbols in this dataset">
      <span className="faint">In this dataset:</span>
      {symbols.map((s) => <button key={s} type="button" className="chip-toggle" aria-pressed={have.includes(s)} onClick={() => toggle(s)}>{s}</button>)}
      {missing.length > 0 && (
        <span className="lab-symbol-warn" role="status">
          Not in this dataset: {missing.map((m, i) => {
            const near = symbols.find((s) => s.toUpperCase() === m.toUpperCase());
            return <span key={m}>{i ? ", " : ""}<b>{m}</b>{near ? <> (did you mean <button type="button" className="linkish" onClick={() => onChange(have.map((x) => (x === m ? near : x)).join(","))}>{near}</button>?)</> : null}</span>;
          })}. The engine accepts {missing.length === 1 ? "it" : "them"}, but no row will match, so {missing.length === 1 ? "it makes" : "they make"} no requests.
        </span>
      )}
    </div>
  );
}
