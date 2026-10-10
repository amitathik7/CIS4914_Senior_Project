import { useNavigate } from "react-router";
import { draftFromRequest, handoffBlocker, toCarried, type HandoffState } from "../../../strategies/backtestHandoff";
import { specOf } from "../../../strategies/catalog";
import type { Member } from "../../../strategies/compare";
import { compareDataset, memberCheck, memberIssues, memberSpec, type CompareState } from "../../../strategies/compareState";
import { validateDraft, type StrategyDraft } from "../../../strategies/draft";
import { useEngine } from "../../../state/engine";

/** One way of carrying a side to the New backtest form. Nothing starts there either. */
export interface HandoffOption {
  /** Why it cannot be carried, in a sentence. */
  blocked: string | undefined;
  go: () => void;
  /** Which settings it carries, in words ("A as currently edited"). */
  basis: string;
}

/**
 * "Use A / B in new backtest". A side has two possible sources, and they differ once it is edited after a run:
 *   edited   the settings in that side's editor now (what the person sees and can still change);
 *   compared the settings the comparison on screen actually ran (present only when there is a comparison).
 * Each is carried exactly as its parameter text reads, or refused with the reason (see handoffBlocker).
 */
export function useHandoff(state: CompareState, member: Member): { edited: HandoffOption; compared: HandoffOption | undefined } {
  const navigate = useNavigate();
  const { info } = useEngine();
  const coverage = info?.market_data.symbols;

  const option = (draft: StrategyDraft, title: string | undefined, basis: string, dataset: string | undefined, blocked: string | undefined): HandoffOption => ({
    basis,
    blocked,
    go: () => {
      const carried = toCarried(draft);
      if (!carried || blocked) return;
      const handoff: HandoffState = { fromStrategies: carried, label: `${title ?? carried.kind} from Compare, configuration ${member}`, basis, dataset };
      navigate("/backtests/new", { state: handoff });
    },
  });

  const spec = memberSpec(state, member);
  const draft = state.members[member].draft;
  const check = memberCheck(state, member);
  const editedBlocker = spec
    ? handoffBlocker(draft, spec, memberIssues(state, member).length, check.phase === "rejected" ? check.error.message : undefined, coverage)
    : "Choose a strategy first.";
  const edited = option(draft, spec?.title, `${member} as currently edited`, compareDataset(state)?.name, editedBlocker);

  const done = state.comparison;
  const ran = done?.snapshots[member];
  if (!done || !ran) return { edited, compared: undefined };
  const ranDraft = draftFromRequest(ran.request);
  const ranSpec = specOf(state.lab.catalog, ran.request.kind);
  // It ran, so the engine has already accepted it: only the form's own limits can refuse it.
  const ranBlocker = handoffBlocker(ranDraft, ranSpec, ranSpec ? validateDraft(ranSpec, ranDraft).length : 0, undefined, coverage);
  return { edited, compared: option(ranDraft, ran.strategyTitle, `${member} as it ran in comparison ${done.id}`, ran.request.dataset.name, ranBlocker) };
}
