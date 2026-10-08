import { useSyncExternalStore } from "react";

export type Theme = "dark" | "light";

const root = () => document.documentElement;

function subscribe(callback: () => void) {
  const observer = new MutationObserver(callback);
  observer.observe(root(), { attributes: true, attributeFilter: ["data-theme"] });
  return () => observer.disconnect();
}

const current = (): Theme => (root().dataset.theme === "light" ? "light" : "dark");

export function useTheme(): Theme {
  return useSyncExternalStore(subscribe, current);
}

export function setTheme(theme: Theme) {
  root().dataset.theme = theme;
  try {
    localStorage.setItem("te.theme", theme);
  } catch {
    // The choice still applies for this page.
  }
}
