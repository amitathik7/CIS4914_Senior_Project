import { useEffect, useSyncExternalStore } from "react";
import { createLabClient, type LabClient } from "../api/lab";
import { LabStore, type LabState } from "./store";

/** The one client of the Strategy Lab gateway: Configure, Explore, Compare and Validation all talk to it through this. */
export const labApi: LabClient = createLabClient();

/** One store for the whole tab, so the draft, the dataset and the last result survive moving between pages and runs. */
export const labStore = new LabStore(labApi);

export function useLab(): LabState {
  return useSyncExternalStore(labStore.subscribe, labStore.getState);
}

/** Connect to the gateway the first time a Strategies page is shown, and notice when the engine was rebuilt (on focus). */
export function useLabConnection(): void {
  const phase = useLab().service.phase;
  useEffect(() => {
    if (phase === "unknown") void labStore.connect();
  }, [phase]);
  useEffect(() => {
    const onFocus = () => void labStore.pollStatus();
    window.addEventListener("focus", onFocus);
    return () => window.removeEventListener("focus", onFocus);
  }, []);
}
