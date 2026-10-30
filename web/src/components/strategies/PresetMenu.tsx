import { useEffect, useRef, useState, type FormEvent } from "react";
import type { Catalog } from "../../strategies/catalog";
import { specOf } from "../../strategies/catalog";
import type { StrategyDraft } from "../../strategies/draft";
import type { ExamplePreset, Preset } from "../../strategies/presets";
import { IconBookmark, IconChevronDown, IconTrash } from "../icons";
import { Notice } from "../ui";

/** What the menu needs from whoever owns the draft: Configure's single draft, or one side of Compare. */
export interface PresetHost {
  presets: readonly Preset[];
  catalog: Catalog | undefined;
  /** Example parameters for the chosen dataset (demonstrations, not recommendations). */
  examples: readonly ExamplePreset[];
  save: (name: string) => { ok: boolean; persisted: boolean; message?: string };
  load: (id: string) => boolean;
  remove: (id: string) => void;
  useExample: (draft: StrategyDraft, label: string) => void;
}

/** Saved presets live in this browser's localStorage (like the theme and the demo backtests): the menu says so. `side` names a Compare configuration. */
export function PresetMenu({ host, side }: { host: PresetHost; side?: string }) {
  const [open, setOpen] = useState(false);
  const [name, setName] = useState("");
  const [message, setMessage] = useState<{ tone?: "down" | "warn"; text: string }>();
  const box = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!open) return;
    const close = (e: MouseEvent) => !box.current?.contains(e.target as Node) && setOpen(false);
    const esc = (e: KeyboardEvent) => e.key === "Escape" && setOpen(false);
    document.addEventListener("mousedown", close);
    document.addEventListener("keydown", esc);
    return () => {
      document.removeEventListener("mousedown", close);
      document.removeEventListener("keydown", esc);
    };
  }, [open]);

  const save = (e: FormEvent) => {
    e.preventDefault();
    const result = host.save(name);
    if (!result.ok) setMessage({ tone: "down", text: result.message ?? "Could not save." });
    else {
      setMessage(result.persisted ? { text: `Saved “${name.trim()}” in this browser.` } : { tone: "warn", text: "Saved for this page only: this browser would not store it (storage is disabled or full)." });
      setName("");
    }
  };

  const who = side ? ` ${side}` : "";
  return (
    <div className="menu-wrap" ref={box}>
      <button type="button" className="btn btn-sm" aria-haspopup="dialog" aria-expanded={open} aria-label={`Presets${side ? ` for ${side}` : ""}`} onClick={() => setOpen((o) => !o)}>
        <IconBookmark size={13} /> Presets <IconChevronDown size={12} />
      </button>
      {open && (
        <div className="popover lab-menu" role="dialog" aria-label={`Configuration presets${side ? ` for ${side}` : ""}`}>
          <form onSubmit={save} className="stack" style={{ gap: 6, padding: "6px 8px 10px" }}>
            <label className="field">
              <span>Save the current configuration{who} as</span>
              <div className="row">
                <input className="input" value={name} maxLength={60} placeholder="Preset name" onChange={(e) => setName(e.target.value)} aria-label="Preset name" />
                <button type="submit" className="btn btn-primary btn-sm" disabled={!name.trim()}>Save</button>
              </div>
              <small>Stored in this browser's localStorage only. Presets are not shared with your team and do not follow you to another browser. Saving under an existing name replaces it.</small>
            </label>
            {message && <Notice tone={message.tone}>{message.text}</Notice>}
          </form>
          <div className="popover-label">Your presets</div>
          {host.presets.length === 0 && <p className="faint" style={{ padding: "4px 10px 8px" }}>None yet.</p>}
          {host.presets.map((p) => (
            <div key={p.id} className="lab-preset">
              <button type="button" className="run-option" onClick={() => { if (host.load(p.id)) setOpen(false); }}>
                <strong>{p.name}</strong>
                <small>{specOf(host.catalog, p.draft.kind)?.title ?? p.draft.kind} · saved {new Date(p.savedAt).toLocaleDateString("en-US", { month: "short", day: "numeric" })}</small>
              </button>
              <button type="button" className="btn btn-ghost btn-sm icon-btn" aria-label={`Delete preset ${p.name}`} onClick={() => host.remove(p.id)}><IconTrash size={13} /></button>
            </div>
          ))}
          {host.examples.length > 0 && (
            <>
              <div className="popover-label">Examples for this dataset</div>
              <p className="faint lab-help" style={{ padding: "0 10px 6px" }}>Demo parameters from the lab's own tests, not recommendations.</p>
              {host.examples.map((x) => (
                <button key={x.id} type="button" className="run-option" onClick={() => { host.useExample(x.draft, `Example: ${x.name}`); setOpen(false); }}>
                  <strong>{x.name}</strong>
                  <small>Example · loads into the {side ? `configuration ${side}` : "draft"}, nothing runs</small>
                </button>
              ))}
            </>
          )}
        </div>
      )}
    </div>
  );
}
