# Testing

## Running

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
```

The test binary is `co_tests`; `ctest` runs one test per suite plus the package proof. A suite can
be run directly:

```sh
co_tests --list                # every test case
co_tests --suite assess        # one suite
co_tests --filter utilization  # name filter
```

## Suites

| Suite | Tests | What it establishes |
|---|---|---|
| `core` | 18 | names, typed ids, digest ids, time and ISO-8601, checked arithmetic, hashing, CRC-64 check value, JSON writer/parser bounds, limits, cancellation |
| `model` | 9 | topology construction, indexing, digests, builder rejections, limits, generation ordering, every fence decision class |
| `evidence` | 13 | kind to role mapping, evidence identity, validation, every freshness state, agreement including minorities and incomparability, store fencing, retirement, eviction, sweeps, query determinism, recovery |
| `assess` | 17 | utilization alone, stale pressure, fresh pressure, confirmed congestion, marginal and unusable impairment, latency baseline, saturation, contention, conflicting sources, unsupported and incomplete evidence, generation mismatch, recovered evidence, no evidence, determinism, citation bounds, severity |
| `localize_episode` | 15 | edges require evidence, symptom must be asserted, structural-only rejection, localization, ambiguity, depth bounds, episode identity, lifecycle, history bounds, budgets, correlation determinism, over-merge refusal, cancellation |
| `persistence` | 7 | round trip, corruption of every region, version and configuration mismatches, conservative store recovery, directory creation, bounds, identity verification |
| `runtime` | 9 | end to end ingest/classify/localize/episode/correlate/explain/export, unknown subjects, configuration validation, shutdown, worker pool, maintenance, topology admission, causal edges, structural edges requiring advertisement |
| `restart` | 5 | history survives and liveness does not, corrupt primary falls back, unusable snapshots leave the runtime empty, policy and limits changes are reported, persistence disabled |
| `transport` | 6 | frame round trip and refusals, in-process server and client, rejected records, a real second process as the client, a real second process as the server, refused connections |
| `property` | 6 | seeded randomized classification invariants, reproducibility including permutation, recovered and retired evidence, episode history reproducibility, store bounds, agreement symmetry |
| `adversarial` | 9 | hostile JSON, malformed evidence documents, incoherent requests, extreme values, snapshot fuzzing and truncation, hostile frame streams, store limits, episode and correlation budgets, forged identities |
| `concurrency` | 5 | parallel ingest, parallel analysis during ingest, worker pool draining, cancellation, consistent reads |
| `hardening` | 6 | hostile peer, reordered delivery, incomparable generation, whole-surface concurrency, section-less snapshots, repeated restarts |
| `endtoend` | 4 | CLI selfcheck and limits, topology validation, the full pipeline from the command line, malformed invocations |

123 + 6 = 129 test cases.

## No timeouts

There is no timeout mechanism anywhere in the test framework, and no test depends on one to pass.
The transport layers use operation deadlines as a product feature (a collector must not block
forever on a silent peer); tests configure them generously so that a defect surfaces as a reported
protocol error rather than as a hang. Process-based tests synchronise on the child process itself
(`running()`) instead of sleeping.

## Coverage of the proof surfaces

* **Determinism** – `assess.classification_is_deterministic`,
  `property.classification_is_reproducible_for_every_seed` (including record permutation),
  `localize_episode.correlation_groups_related_episodes_deterministically`,
  `property.episode_history_is_reproducible_from_the_same_observation_sequence`.
* **Utilization is not congestion** – `assess.utilization_alone_is_never_congestion`,
  `endtoend.full_pipeline_from_the_command_line`, `tests/downstream/consumer.cpp`.
* **Stale evidence cannot prove the present** – `assess.stale_pressure_cannot_prove_current_congestion`,
  `assess.recovered_evidence_is_history_not_liveness`, `property.recovered_and_retired_evidence_never_asserts`.
* **Causal edges need evidence** – `localize_episode.causal_edges_always_require_evidence`,
  `localize_episode.structural_adjacency_alone_is_never_a_culprit`,
  `runtime.structural_edges_need_a_topology_observation`.
* **Conflicting sources stay visible** – `assess.conflicting_sources_stay_visible`,
  `evidence.agreement_keeps_minorities_visible`.
* **Grouping is deterministic and does not over-merge** –
  `localize_episode.correlation_refuses_to_over_merge`.
* **Restart preserves history, not liveness** – `restart.history_survives_and_liveness_does_not`,
  `hardening.repeated_restart_cycles_converge`.
