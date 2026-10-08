import { useMemo, useState, type KeyboardEvent, type ReactNode } from "react";
import { IconArrowDown, IconArrowUp } from "./icons";

export interface Column<T> {
  key: string;
  header: string;
  render: (row: T) => ReactNode;
  sort?: (row: T) => number | string;
  align?: "right";
  width?: number | string;
}

interface Props<T> {
  rows: T[];
  columns: Column<T>[];
  rowKey: (row: T) => string | number;
  onRowClick?: (row: T) => void;
  selectedKey?: string | number;
  initialSort?: { key: string; dir: "asc" | "desc" };
  empty?: ReactNode;
  maxHeight?: number;
  limit?: number;
  label: string;
}

export function DataTable<T>({ rows, columns, rowKey, onRowClick, selectedKey, initialSort, empty, maxHeight, limit, label }: Props<T>) {
  const [sort, setSort] = useState(initialSort);

  const sorted = useMemo(() => {
    const col = columns.find((c) => c.key === sort?.key);
    if (!col?.sort || !sort) return rows;
    const by = col.sort;
    const dir = sort.dir === "asc" ? 1 : -1;
    return [...rows].sort((a, b) => {
      const x = by(a);
      const y = by(b);
      return (x < y ? -1 : x > y ? 1 : 0) * dir;
    });
  }, [rows, columns, sort]);

  const visible = limit ? sorted.slice(0, limit) : sorted;

  const toggle = (key: string) =>
    setSort((s) => (s?.key === key ? { key, dir: s.dir === "asc" ? "desc" : "asc" } : { key, dir: "desc" }));

  const onKey = (e: KeyboardEvent, row: T) => {
    if (onRowClick && (e.key === "Enter" || e.key === " ")) {
      e.preventDefault();
      onRowClick(row);
    }
  };

  if (rows.length === 0 && empty) return <>{empty}</>;

  return (
    <div className="table-wrap" style={maxHeight ? { maxHeight } : undefined}>
      <table className="table" aria-label={label}>
        <thead>
          <tr>
            {columns.map((c) => {
              const active = sort?.key === c.key;
              return (
                <th
                  key={c.key}
                  className={c.align === "right" ? "r" : undefined}
                  style={c.width ? { width: c.width } : undefined}
                  aria-sort={active ? (sort.dir === "asc" ? "ascending" : "descending") : undefined}
                  scope="col"
                >
                  {c.sort ? (
                    <button type="button" onClick={() => toggle(c.key)}>
                      {c.header}
                      {active && (sort.dir === "asc" ? <IconArrowUp size={11} /> : <IconArrowDown size={11} />)}
                    </button>
                  ) : (
                    c.header
                  )}
                </th>
              );
            })}
          </tr>
        </thead>
        <tbody>
          {visible.map((row) => {
            const key = rowKey(row);
            return (
              <tr
                key={key}
                data-clickable={onRowClick ? "true" : undefined}
                aria-selected={selectedKey === key ? true : undefined}
                tabIndex={onRowClick ? 0 : undefined}
                onClick={onRowClick ? () => onRowClick(row) : undefined}
                onKeyDown={onRowClick ? (e) => onKey(e, row) : undefined}
              >
                {columns.map((c) => (
                  <td key={c.key} className={c.align === "right" ? "r" : undefined}>
                    {c.render(row)}
                  </td>
                ))}
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}
