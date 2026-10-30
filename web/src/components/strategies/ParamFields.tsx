import type { ReactNode } from "react";
import type { StrategySpec } from "../../strategies/catalog";
import { paramCopy } from "../../strategies/copy";
import type { FieldIssue } from "../../strategies/draft";
import { ParamField } from "./ParamField";

/**
 * One strategy's parameters: the everyday fields, an Advanced disclosure, and the raw parameter list. Shared by Configure and by each
 * side of Compare. `skip` leaves out fields that are edited elsewhere (Compare's shared symbol allowlist).
 */
export function ParamFields({ spec, values, issues, flagged, onChange, skip = [], symbolChips }: {
  spec: StrategySpec;
  values: Readonly<Record<string, string>>;
  issues: readonly FieldIssue[];
  /** Parameters the engine's own rejection message names. */
  flagged: readonly string[];
  onChange: (name: string, text: string) => void;
  skip?: readonly string[];
  symbolChips?: (value: string) => ReactNode;
}) {
  const issueOf = Object.fromEntries(issues.map((i) => [i.name, i.message]));
  const shown = spec.params.filter((p) => !skip.includes(p.name));
  const regular = shown.filter((p) => !paramCopy(p.name).advanced);
  const advanced = shown.filter((p) => paramCopy(p.name).advanced);
  return (
    <>
      <div className="lab-fields">
        {regular.map((p) => (
          <ParamField
            key={`${spec.kind}.${p.name}`} spec={p} value={values[p.name] ?? ""} issue={issueOf[p.name]} flagged={flagged.includes(p.name)}
            wide={p.type === "string_list"} onChange={(t) => onChange(p.name, t)}
            extra={p.type === "string_list" ? symbolChips?.(values[p.name] ?? "") : undefined}
          />
        ))}
      </div>
      {advanced.length > 0 && (
        <details className="lab-details">
          <summary>Advanced</summary>
          <div className="lab-fields" style={{ marginTop: 10 }}>
            {advanced.map((p) => <ParamField key={`${spec.kind}.${p.name}`} spec={p} value={values[p.name] ?? ""} issue={issueOf[p.name]} flagged={flagged.includes(p.name)} wide onChange={(t) => onChange(p.name, t)} />)}
          </div>
        </details>
      )}
      <details className="lab-details">
        <summary>Parameter details</summary>
        <dl className="kv" style={{ marginTop: 8 }}>
          {spec.params.map((p) => (
            <div key={p.name} style={{ display: "contents" }}>
              <dt className="id">{p.name}</dt>
              <dd className="wrap">{p.type}{p.required ? ", required" : ""}{p.defaultText !== undefined ? `, default ${p.defaultText}` : ""}. {p.constraint}</dd>
            </div>
          ))}
        </dl>
      </details>
    </>
  );
}
