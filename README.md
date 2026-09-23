# Congestion Observatory

Vendor-neutral congestion evidence aggregation and causal localization for fabric operations.

Congestion Observatory owns one job: **aggregating network-wide congestion evidence and localizing
its cause**. It never controls congestion — it does not change rates, reroute flows, program queues,
shed traffic or mark packets — and it never treats high utilization on its own as proof of
congestion.

```
                observations                         verdict + citations + blockers
 collectors ──────────────────▶ evidence store ──▶ classification ──▶ episodes
 (documents, frames)            fencing,           rules, features,   history,
                                freshness,         confidence         correlation
                                agreement              │
                                                       ▼
                                            causal localization graph
```

## What it does

* **Typed identities** for nodes, ports, links, queues, buffers, paths, flows, tenants, classes,
  sources and topologies, plus derived identities for evidence, episodes and correlation groups.
* **Evidence ingestion** with explicit provenance: source, authority level, boot id, incarnation,
  epoch/generation/revision, sequence, observation time, receive time and clock domain.
* **Fencing** of stale epoch, stale generation, stale revision, replayed sequence, stale
  incarnation, inconsistent boot, incomparable generation and low authority replays. A reboot
  retires the previous incarnation's evidence: it stays as history and can never support a current
  verdict.
* **Freshness, agreement, completeness and support** as distinct states — fresh, aging, stale,
  expired, unknown; unanimous, minority, conflicting, incomparable, single source; complete,
  partial, incomplete, unknown; supported, unsupported, unknown. Absence of evidence is never
  positive evidence.
* **Classification** into eight verdicts: `no_evidence`, `idle`, `utilized_healthy`,
  `contention_observed`, `pressure_observed`, `saturated`, `congestion_confirmed`,
  `indeterminate`. Only `congestion_confirmed` asserts harm, and only fresh impairment evidence
  can produce it.
* **Evidence-backed causal localization** with ranked candidates, explicit ambiguity, depth bounds
  and refusal of structural-only explanations. A causal edge without citations cannot be created.
* **Episodes** with deterministic identity, a monotonic lifecycle, bounded history that reports
  what it dropped, and deterministic correlation grouping that records every merge and every
  refusal to merge.
* **Persistence** with a versioned, CRC-64 checked snapshot format, primary/fallback rotation,
  identity re-verification on load and conservative recovery. Restart preserves history, never
  liveness.
* **Transport** for remote collectors: a bounded frame protocol over loopback TCP with a
  connection limit, real deadlines and no silent stalling.
* **Tooling**: a command line program for ingest, classify, localize, episodes, episode, advance,
  explain, export, save, load, serve, push, selfcheck and bench.

## What it does not do

* No congestion control of any kind.
* No switch, ASIC, RDMA, InfiniBand, NVLink, multi-host or vendor SDK integration. There is no
  hardware access in this repository: the runtime consumes documents and frames a collector would
  produce.
* No distributed consensus: one process owns its state; several collectors may push to it.
* No cryptographic authentication, and the identity digests are not cryptographic.
* No claim that utilization, contention, pressure or saturation *are* congestion. They are
  different findings with different names, and the API keeps them apart.

See `docs/boundaries.md` for the full REAL / SYNTHETIC / UNSUPPORTED statement.

## Quick start

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The example programs walk through the core flows:

```sh
build/release/examples/example_minimal_pipeline       # utilization is not congestion
build/release/examples/example_localize_episode       # find the upstream cause
build/release/examples/example_persist_and_recover    # restart semantics
```

### Library

```cpp
#include "congestion/congestion.hpp"

using namespace congestion;

TopologyBuilder builder;
builder.set_generation(GenerationVector{Epoch(1), Generation(1), Revision(1)});
// ... add nodes, ports, links, queues, paths, flows ...
Topology topology = builder.build(Limits{}).value();

RuntimeConfig config;
config.topology = topology;
auto runtime = Runtime::create(std::move(config)).value();

runtime->ingest(observation, now);                    // fenced, freshness evaluated
auto assessment = runtime->classify(link, Duration::from_seconds(10), now).value();
if (assessment.congestion_asserted) {
  auto localization = runtime->localize(link, Duration::from_seconds(10), now).value();
}
```

### Command line

```sh
co topology --file fabric.json
co ingest   --file evidence.json --topology fabric.json --state ./state --save
co classify --subject link:leaf1-leaf2 --window 30s --now @1700000001 \
            --file fresh.json --topology fabric.json --state ./state
co localize --subject link:leaf1-leaf2 --window 30s --file fresh.json --state ./state
co episodes --state ./state
co explain  --subject link:leaf1-leaf2 --file fresh.json --state ./state
co export   --evidence --state ./state --out state.json
co serve    --port 45913 --topology fabric.json --state ./state
co push     --endpoint 127.0.0.1:45913 --file evidence.json
co selfcheck
```

Analysis commands accept `--file` so that a restart plus a fresh observation is a first-class
workflow: recovered evidence is history, and only a new observation restores liveness.

## Guarantees the test suite proves

| Guarantee | Where |
|---|---|
| Utilization alone is never congestion | `assess.utilization_alone_is_never_congestion`, `endtoend.full_pipeline_from_the_command_line`, downstream consumer |
| Stale pressure cannot prove current congestion | `assess.stale_pressure_cannot_prove_current_congestion` |
| Persisted evidence never becomes fresh after a restart | `restart.history_survives_and_liveness_does_not`, `assess.recovered_evidence_is_history_not_liveness` |
| Causal edges require explicit evidence | `localize_episode.causal_edges_always_require_evidence`, `runtime.structural_edges_need_a_topology_observation` |
| Conflicting sources stay visible | `assess.conflicting_sources_stay_visible`, `evidence.agreement_keeps_minorities_visible` |
| Grouping is deterministic and does not over-merge | `localize_episode.correlation_refuses_to_over_merge` |
| Classification and episode history are reproducible | `assess.classification_is_deterministic`, `property.*` |
| Every replay class is fenced | `model.fence_rejects_every_replay_class` |
| Snapshots are integrity checked and recovered conservatively | `persistence.*`, `restart.*`, `adversarial.snapshot_fuzzing_never_crashes_or_silently_accepts` |
| Distributed behaviour is real, in real processes | `transport.independent_process_client_pushes_into_this_process`, `transport.this_process_pushes_into_an_independent_server_process` |

## Documentation

| Document | Contents |
|---|---|
| `docs/architecture.md` | layering, ownership, the lock graph, bounds, error model |
| `docs/evidence-model.md` | the record, the role table, freshness, agreement |
| `docs/identity.md` | canonical names, derived identities, generation and fencing |
| `docs/classification.md` | verdicts, rule order, impairment thresholds, determinism |
| `docs/localization.md` | graph rules, candidate scoring, outcomes |
| `docs/persistence.md` | snapshot format, recovery, restart semantics |
| `docs/limits.md` | every bound and what happens at it |
| `docs/testing.md` | suites, coverage of the proof surfaces, no-timeout policy |
| `docs/hardening.md` | every defect found during hardening and how it was fixed |
| `docs/boundaries.md` | REAL / SYNTHETIC / UNSUPPORTED |
| `docs/cli.md` | command reference and document schemas |

## Building

Requirements: CMake 3.25+, a C++20 compiler, and threads. Windows x64 with MSVC 19.44 is the
validated configuration; the build also configures GCC/Clang warning sets and POSIX sockets, but no
Linux or macOS build was executed here.

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release
```

Options: `CONGESTION_BUILD_TOOLS`, `CONGESTION_BUILD_TESTS`, `CONGESTION_BUILD_EXAMPLES`,
`CONGESTION_BUILD_BENCHMARKS`, `CONGESTION_ENABLE_ASAN`, `CONGESTION_WARNINGS_AS_ERRORS`.

First-party warning count is zero in every configuration (`/W4 /WX /permissive-`).

### Consuming the package

```cmake
find_package(CongestionObservatory 1.0 REQUIRED CONFIG)
target_link_libraries(my_collector PRIVATE CongestionObservatory::congestion_observatory)
```

The repository proves this from an independent project: `ctest -R package.downstream` installs the
package into a scratch prefix and then configures, builds and runs `tests/downstream`, which knows
nothing about this build tree.

## Benchmarks

```sh
build/release/benchmarks/bench_observatory --records 20000 --links 16
```

The benchmark reports completed work (records actually ingested, classifications actually performed,
snapshot bytes actually written) and exits non-zero if any of it was dropped. It is a measurement of
this implementation, not a comparison with any other system.

## Status

Version 1.0.0, Apache-2.0, first release. See `CHANGELOG.md`.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
