import { useSyncExternalStore } from "react";
import { CompareStore } from "./compareStore";
import type { CompareState } from "./compareState";
import { labApi, labStore } from "./useLab";
import { ValidationStore, type ValidationState } from "./validationStore";

/** One store each for the whole tab, so the two configurations, the last comparison and the last validation survive moving between pages. */
export const compareStore = new CompareStore(labApi, labStore);
export const validationStore = new ValidationStore(labApi, labStore);

export function useCompare(): CompareState {
  return useSyncExternalStore(compareStore.subscribe, compareStore.getState);
}

export function useValidation(): ValidationState {
  return useSyncExternalStore(validationStore.subscribe, validationStore.getState);
}
