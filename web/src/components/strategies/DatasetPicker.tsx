import { useId, useRef } from "react";
import type { BuiltinDataset } from "../../api/lab";
import type { DatasetSelection } from "../../strategies/store";
import type { UploadedFile } from "../../strategies/uploads";
import { IconUpload } from "../icons";
import { Badge, Notice } from "../ui";

/** What the picker shows and does: Configure's dataset, or the one Compare's two sides share. */
export interface DatasetHost {
  datasets: readonly BuiltinDataset[];
  dataset: DatasetSelection;
  upload?: UploadedFile;
  uploadError?: string;
  selectBuiltin: (key: string) => void;
  selectUpload: () => void;
  uploadFile: (file: File) => Promise<boolean>;
  removeUpload: () => void;
}

/** Built-in fixtures or a CSV from disk. Whether a file is a valid dataset is the replay tool's call, made when you run. */
export function DatasetPicker({ host, label = "Dataset" }: { host: DatasetHost; label?: string }) {
  const file = useRef<HTMLInputElement>(null);
  const selectId = useId();
  const { datasets, dataset, upload, uploadError } = host;
  const current = dataset.kind === "builtin" ? datasets.find((d) => d.key === dataset.key) : undefined;

  const pick = async (files: FileList | null) => {
    const chosen = files?.[0];
    if (chosen) await host.uploadFile(chosen);
    if (file.current) file.current.value = ""; // choosing the same file again must still fire
  };

  return (
    <div className="stack" style={{ gap: 8 }}>
      <label className="field" htmlFor={selectId}>
        <span>{label}</span>
        <select
          id={selectId} className="select"
          value={dataset.kind === "upload" ? "__upload__" : dataset.key}
          onChange={(e) => (e.target.value === "__upload__" ? host.selectUpload() : host.selectBuiltin(e.target.value))}
        >
          {datasets.map((d) => <option key={d.key} value={d.key} disabled={!d.available}>{d.label}{d.available ? "" : " (file missing)"}</option>)}
          {upload && <option value="__upload__">Uploaded: {upload.name}</option>}
        </select>
      </label>
      <div className="row" style={{ flexWrap: "wrap" }}>
        <button type="button" className="btn btn-sm" onClick={() => file.current?.click()}><IconUpload size={13} /> Upload CSV</button>
        <input ref={file} type="file" accept=".csv,text/csv,text/plain" hidden aria-label="Upload a CSV file" onChange={(e) => void pick(e.target.files)} />
        {upload && <button type="button" className="btn btn-ghost btn-sm" onClick={() => host.removeUpload()}>Remove upload</button>}
      </div>
      {uploadError && <Notice tone="down">{uploadError}</Notice>}

      {dataset.kind === "builtin" && current && (
        <div className="lab-dataset">
          <div className="row" style={{ flexWrap: "wrap" }}>
            <Badge>Synthetic fixture</Badge>
            <span className="faint">{current.rows} rows · {current.symbols?.join(", ")}</span>
          </div>
          <p className="faint">{current.description}</p>
        </div>
      )}
      {dataset.kind === "upload" && upload && (
        <div className="lab-dataset">
          <div className="row" style={{ flexWrap: "wrap" }}>
            <Badge tone="warn">Provenance unknown</Badge>
            <span className="faint">{(upload.size / 1024).toFixed(upload.size < 10240 ? 1 : 0)} KiB{upload.symbols.length ? ` · ${upload.symbols.join(", ")}` : ""}</span>
          </div>
          <p className="faint">Uploaded in this browser session. The lab cannot tell where these bars came from. SHA-256 <span className="id">{upload.sha256.slice(0, 16)}…</span></p>
        </div>
      )}
      <details className="lab-details">
        <summary>CSV format</summary>
        <p className="faint lab-help" style={{ marginTop: 6 }}>Columns symbol, exchange_time (UTC, ending in Z), type (bar or trade) and price; optionally open, high, low and volume. Prices are plain decimals with at most six places. The replay tool checks every row and reports the line and column of the first problems.</p>
      </details>
    </div>
  );
}
