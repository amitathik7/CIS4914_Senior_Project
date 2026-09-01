# Issue templates

Four structured forms plus `config.yml`. When someone clicks **New issue** on
GitHub they pick one of these.

| File | Use for | Title prefix | Labels applied |
|------|---------|--------------|----------------|
| `feature_implementation.yml` | Turning a `NotImplemented` stub into real behaviour, or adding a planned capability | `[Feature]:` | `feature`, `needs-triage` |
| `defect_report.yml` | A bug: wrong result, crash, hang, flaky test, build break, stub reached where it shouldn't be | `[Bug]:` | `bug`, `needs-triage` |
| `architecture_decision.yml` | Resolving an `OPEN_QUESTIONS.md` item into an ADR | `[ADR]:` | `adr`, `discussion`, `needs-triage` |
| `performance_experiment.yml` | A measured latency / throughput / memory experiment | `[Perf]:` | `performance`, `experiment`, `needs-triage` |
| `config.yml` | Not a form — keeps blank issues enabled and adds contact links | — | — |

## Prerequisite: create the labels first

**The templates do not create labels.** GitHub **silently drops** any label a
template references that does not already exist in the repository — the issue is
still created, just without the label, and nobody notices.

Before relying on these templates, create the full label set under
**Settings → Labels** (or `gh label create ...`):

| Label | Meaning |
|-------|---------|
| `feature` | Planned functionality / a stub becoming real |
| `bug` | Defect |
| `adr` | Architecture decision to be recorded |
| `discussion` | Needs team input before it can proceed |
| `performance` | Latency / throughput / memory work |
| `experiment` | A measured trial with a hypothesis and a result |
| `needs-triage` | Not yet reviewed / assigned by the team |

Optional but useful (not applied automatically by any template — the bug form
uses a *dropdown* for severity, not a label): `severity:blocker`,
`severity:major`, `severity:minor`, `severity:trivial`, and one label per
milestone (`M1` … `M10`) if you want to filter the backlog by milestone.

Part of the **M1 "Repo & CI setup"** work package in
[`../../docs/IMPLEMENTATION_PLAN.md`](../../docs/IMPLEMENTATION_PLAN.md).

## Editing a form

GitHub issue-form schema:
<https://docs.github.com/en/communities/using-templates-to-encourage-useful-issues-and-pull-requests/syntax-for-githubs-form-schema>.
Validate YAML before committing (`python -c "import yaml,sys;yaml.safe_load(open(sys.argv[1]))" <file>`).
