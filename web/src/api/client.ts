import type {
  ApiError, BacktestRequest, Bar, EngineInfo, EquityPoint, Fill, Order, PortfolioSnapshot,
  RiskDecision, RunDetail, RunEvent, RunSummary, SystemMetrics, Trade, TradeSignal,
} from "./contract";

export interface EngineClient {
  info(): Promise<EngineInfo>;
  listRuns(): Promise<RunSummary[]>;
  getRun(runId: string): Promise<RunDetail>;
  createBacktest(request: BacktestRequest): Promise<RunSummary>;
  stopRun(runId: string): Promise<RunSummary>;
  setKillSwitch(runId: string, engaged: boolean, reason?: string): Promise<RunDetail>;
  portfolio(runId: string): Promise<PortfolioSnapshot>;
  equity(runId: string): Promise<EquityPoint[]>;
  bars(runId: string, symbol: string): Promise<Bar[]>;
  signals(runId: string): Promise<TradeSignal[]>;
  riskDecisions(runId: string): Promise<RiskDecision[]>;
  orders(runId: string): Promise<Order[]>;
  fills(runId: string): Promise<Fill[]>;
  trades(runId: string): Promise<Trade[]>;
  metrics(runId: string): Promise<SystemMetrics>;
  subscribe(runId: string, onEvent: (event: RunEvent) => void): () => void;
}

export class EngineApiError extends Error {
  constructor(readonly status: number, readonly code: string, message: string, readonly field?: string) {
    super(message);
  }
}

// REST + Server-Sent Events implementation of the contract.
export class HttpEngineClient implements EngineClient {
  constructor(private readonly base: string) {}

  private async request<T>(path: string, init?: RequestInit): Promise<T> {
    const response = await fetch(`${this.base}/v1${path}`, {
      ...init,
      headers: { "Content-Type": "application/json", Accept: "application/json", ...init?.headers },
    });
    if (!response.ok) {
      const body = (await response.json().catch(() => null)) as ApiError | null;
      throw new EngineApiError(
        response.status,
        body?.error.code ?? "http_error",
        body?.error.message ?? `The engine answered ${response.status} ${response.statusText}.`,
        body?.error.field,
      );
    }
    return (await response.json()) as T;
  }

  private run<T>(runId: string, path: string) {
    return this.request<T>(`/runs/${encodeURIComponent(runId)}${path}`);
  }

  info = () => this.request<EngineInfo>("/engine");
  listRuns = () => this.request<RunSummary[]>("/runs");
  getRun = (id: string) => this.run<RunDetail>(id, "");
  createBacktest = (body: BacktestRequest) =>
    this.request<RunSummary>("/runs", { method: "POST", body: JSON.stringify(body) });
  stopRun = (id: string) =>
    this.request<RunSummary>(`/runs/${encodeURIComponent(id)}/stop`, { method: "POST" });
  setKillSwitch = (id: string, engaged: boolean, reason?: string) =>
    this.request<RunDetail>(`/runs/${encodeURIComponent(id)}/kill-switch`, {
      method: "PUT",
      body: JSON.stringify({ engaged, reason }),
    });
  portfolio = (id: string) => this.run<PortfolioSnapshot>(id, "/portfolio");
  equity = (id: string) => this.run<EquityPoint[]>(id, "/equity");
  bars = (id: string, symbol: string) => this.run<Bar[]>(id, `/bars?symbol=${encodeURIComponent(symbol)}`);
  signals = (id: string) => this.run<TradeSignal[]>(id, "/signals");
  riskDecisions = (id: string) => this.run<RiskDecision[]>(id, "/risk-decisions");
  orders = (id: string) => this.run<Order[]>(id, "/orders");
  fills = (id: string) => this.run<Fill[]>(id, "/fills");
  trades = (id: string) => this.run<Trade[]>(id, "/trades");
  metrics = (id: string) => this.run<SystemMetrics>(id, "/metrics");

  // One EventSource per run, shared by every subscriber. A page has several resources on the same run, and a browser
  // allows only six HTTP/1.1 connections per host: one stream each would starve the page's own requests.
  private readonly streams = new Map<string, { source: EventSource; listeners: Set<(event: RunEvent) => void> }>();

  subscribe(runId: string, onEvent: (event: RunEvent) => void) {
    let stream = this.streams.get(runId);
    if (!stream) {
      const source = new EventSource(`${this.base}/v1/runs/${encodeURIComponent(runId)}/stream`);
      const listeners = new Set<(event: RunEvent) => void>();
      source.onmessage = (message) => {
        const event = JSON.parse(message.data as string) as RunEvent;
        for (const listener of listeners) listener(event);
      };
      stream = { source, listeners };
      this.streams.set(runId, stream);
    }
    const current = stream;
    current.listeners.add(onEvent);
    return () => {
      current.listeners.delete(onEvent);
      if (current.listeners.size === 0) {
        current.source.close();
        this.streams.delete(runId);
      }
    };
  }
}
