import { useId } from "react";
import type { ParamSpec } from "../../strategies/catalog";
import { paramCopy } from "../../strategies/copy";

/**
 * One parameter as a labelled input. The friendly name and unit are the console's wording; the raw field name, type, default and the
 * engine's own constraint text sit in the tooltip (and in the "Parameter details" list), so the form stays short.
 */
export function ParamField({ spec, value, issue, flagged, wide, onChange, extra }: {
  spec: ParamSpec;
  value: string;
  /** A problem found by the console's own checks (shown under the field). */
  issue?: string;
  /** The engine's rejection message names this field (marked, with the message shown in the engine check). */
  flagged?: boolean;
  wide?: boolean;
  onChange: (text: string) => void;
  extra?: React.ReactNode;
}) {
  const hintId = useId();
  const copy = paramCopy(spec.name);
  const raw = `${spec.name} (${spec.type}${spec.defaultText !== undefined ? `, default ${spec.defaultText}` : ""}): ${spec.constraint}`;
  return (
    <div className={wide ? "lab-wide" : undefined}>
      <label className="field" data-invalid={issue || flagged ? "true" : undefined} title={raw}>
        <span>
          {copy.label}
          {copy.unit && <span className="lab-unit"> · {copy.unit}</span>}
        </span>
        <input
          className="input"
          value={value}
          inputMode={spec.type === "uint" ? "numeric" : spec.type === "double" ? "decimal" : "text"}
          autoComplete="off"
          spellCheck={false}
          aria-invalid={issue || flagged ? true : undefined}
          aria-describedby={hintId}
          aria-label={copy.label}
          onChange={(e) => onChange(e.target.value)}
        />
        <small id={hintId}>{issue ?? (copy.hint || spec.constraint)}</small>
      </label>
      {extra}
    </div>
  );
}
