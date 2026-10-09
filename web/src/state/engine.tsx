import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { HttpEngineClient, type EngineClient } from "../api/client";
import type { EngineInfo, RunSummary } from "../api/contract";
import { MockEngineClient } from "../mock/mockClient";

interface EngineContextValue {
  client: EngineClient;
  info: EngineInfo | undefined;
  runs: RunSummary[] | undefined;
  refreshRuns: () => void;
  defaultRunId: string | undefined;
}

const EngineContext = createContext<EngineContextValue | null>(null);

function createClient(): EngineClient {
  const base = import.meta.env.VITE_ENGINE_API as string | undefined;
  return base ? new HttpEngineClient(base.replace(/\/$/, "")) : new MockEngineClient();
}

export function EngineProvider({ children }: { children: ReactNode }) {
  const client = useMemo(createClient, []);
  const [info, setInfo] = useState<EngineInfo>();
  const [runs, setRuns] = useState<RunSummary[]>();

  const refreshRuns = useCallback(() => {
    client.listRuns().then(setRuns, () => setRuns([]));
  }, [client]);

  useEffect(() => {
    client.info().then(setInfo, () => undefined);
    refreshRuns();
    const timer = window.setInterval(refreshRuns, 3000);
    return () => window.clearInterval(timer);
  }, [client, refreshRuns]);

  const defaultRunId = runs?.find((r) => r.mode === "live" && r.status === "running")?.run_id ?? runs?.[0]?.run_id;

  return (
    <EngineContext.Provider value={{ client, info, runs, refreshRuns, defaultRunId }}>{children}</EngineContext.Provider>
  );
}

export function useEngine(): EngineContextValue {
  const value = useContext(EngineContext);
  if (!value) throw new Error("useEngine outside EngineProvider");
  return value;
}

// A counter that moves when the run's stream reports something, at most a few times a second.
export function useRunVersion(runId: string | undefined, throttleMs = 400): number {
  const { client } = useEngine();
  const [version, setVersion] = useState(0);
  const pending = useRef<number | null>(null);

  useEffect(() => {
    if (!runId) return;
    const unsubscribe = client.subscribe(runId, () => {
      if (pending.current !== null) return;
      pending.current = window.setTimeout(() => {
        pending.current = null;
        setVersion((v) => v + 1);
      }, throttleMs);
    });
    return () => {
      unsubscribe();
      if (pending.current !== null) window.clearTimeout(pending.current);
      pending.current = null;
    };
  }, [client, runId, throttleMs]);

  return version;
}

export interface Resource<T> {
  data: T | undefined;
  error: Error | undefined;
  loading: boolean;
}

// Loads `load(client)` for a run and reloads whenever the run's stream moves.
export function useRunResource<T>(
  runId: string | undefined,
  load: (client: EngineClient, runId: string) => Promise<T>,
  deps: unknown[] = [],
): Resource<T> {
  const { client } = useEngine();
  const version = useRunVersion(runId);
  const [state, setState] = useState<Resource<T>>({ data: undefined, error: undefined, loading: true });
  const key = `${runId}|${deps.join("|")}`;
  const lastKey = useRef(key);

  useEffect(() => {
    if (!runId) return;
    let cancelled = false;
    const keyChanged = lastKey.current !== key;
    lastKey.current = key;
    if (keyChanged) setState({ data: undefined, error: undefined, loading: true });
    load(client, runId).then(
      (data) => !cancelled && setState({ data, error: undefined, loading: false }),
      (error: Error) => !cancelled && setState((s) => ({ data: s.data, error, loading: false })),
    );
    return () => {
      cancelled = true;
    };
  }, [client, key, version]);

  return state;
}
