# Causal localization

Localization answers "which object explains this symptom", and it refuses to answer when the
evidence does not support an answer.

## The graph

Edges point **from a cause to the symptom it can explain**. The localizer therefore walks *in-edges*
from the symptom towards upstream objects.

Edge kinds: `topology_adjacency`, `containment`, `path_membership`, `queue_on_link`,
`buffer_on_port`, `flow_on_path` (structural) and `demand_upstream`, `shared_contention`,
`latency_propagation`, `buffer_pressure` (observational).

**An edge without citations cannot be created.** `CausalGraph::add_edge` returns
`precondition_failed` when the citation list is empty, and that rule is enforced by the type, by
the CLI, by the runtime and by the tests. Structural relations derived from a topology are only
built when the structure itself was advertised by an observation: a topology is a claim, so
`Runtime::rebuild_causal_graph()` creates a structural edge only for subjects that have a topology
advertisement, oper-state or capacity record, and cites it.

## Candidate scoring

For each reachable upstream object the localizer gathers its own fresh evidence in the window and
scores named contributions:

| Contribution | Points |
|---|---|
| fresh evidence at the candidate | 10 |
| demand observed | 10 |
| demand exceeds capacity | 25 |
| backlog present | 20 |
| backlog ratio above policy | 15 |
| buffer occupancy | 10 |
| contention | 10 |
| impairment also present upstream | 5 |
| observational edge on the path | 10 |
| proximity (1..4 by depth) | 5 - depth |

A candidate with no decisive evidence of its own is rejected and the rejection is recorded in the
explanation: structural adjacency alone never produces a culprit.

## Outcomes

* `localized` – one candidate leads by at least the ambiguity margin;
* `ambiguous` – the leaders are within the margin; **all** of them are reported, never silently
  ranked apart;
* `unlocalized` – no candidate reached the minimum score;
* `not_attempted` – the symptom is not an asserted congestion.

Ranking is deterministic: score descending, then identity ascending. Single-source candidates lose
30% of their confidence. Depth limiting is reported as a blocker rather than hidden.
