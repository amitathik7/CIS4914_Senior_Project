// Brings a freshly finished result into view without hiding it (or the page title above it) under the sticky top bar.
// When the result starts near the top of the page, the page simply goes back to the top, so the title and the view
// switcher stay visible; otherwise the result is placed just below the bar.
export function revealBelowTopBar(el: HTMLElement) {
  const bar = document.querySelector(".topbar")?.getBoundingClientRect().bottom ?? 0;
  const top = el.getBoundingClientRect().top;
  if (top >= bar) return;
  const pageTop = top + window.scrollY;
  const target = pageTop < window.innerHeight / 2 ? 0 : pageTop - bar - 12;
  window.scrollTo({ top: Math.max(0, target), behavior: "smooth" });
}
