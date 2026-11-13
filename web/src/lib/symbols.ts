// Ticker symbols typed by hand (a live run can trade symbols the engine has no recording of).

const SYMBOL = /^[A-Z][A-Z0-9.\-]{0,9}$/;

/** "aapl, msft  spy" -> ["AAPL", "MSFT", "SPY"]; returns the first token that is not a symbol as `invalid`. */
export function parseSymbols(text: string): { symbols: string[]; invalid?: string } {
  const symbols: string[] = [];
  for (const token of text.split(/[\s,]+/).filter(Boolean)) {
    const symbol = token.toUpperCase();
    if (!SYMBOL.test(symbol)) return { symbols, invalid: token };
    if (!symbols.includes(symbol)) symbols.push(symbol);
  }
  return { symbols };
}
