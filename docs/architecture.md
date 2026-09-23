# Architecture

Congestion Observatory is a single-process C++20 runtime with an optional loopback transport. It
owns **network-wide congestion evidence aggregation and causal localization**. It does not control
congestion: it never changes rates, reroutes flows, programs queues, or sheds traffic.

## Layering

```
tools/            congestion-observatory CLI: ingest, classify, localize, episodes, explain,
                  export, save/load, serve, push, selfcheck, bench
transport/        bounded frame protocol, loopback TCP server and client
runtime/          Runtime facade, bounded worker pool, metrics, topology admission
persistence/      versioned, integrity checked snapshots and conservative recovery
episode/          episode identity, lifecycle, bounded history
correlation/      deterministic grouping of episodes, with explicit merge decisions
localization/     evidence-backed causal graph and the localizer
assessment/       versioned policy, feature extraction, rule engine, verdicts
evidence/         observation records, freshness, agreement, bounded store with fencing
model/            typed identities, generation vectors, fencing, topology
core/             result, names, strong ids, time, checked arithmetic, hash, CRC-64, JSON, limits
```

Data flows in one direction: an observation enters through `EvidenceStore`, is fenced, classified
by the rule engine into a verdict, optionally localized through the causal graph, and recorded as
an episode whose history can be grouped, exported and persisted. Nothing in the pipeline can feed
back into the fabric.

## Ownership and locking

| Component | Lock | Held during |
|---|---|---|
| `EvidenceStore` | one `std::mutex` | ingestion, retention, queries (copies out) |
| `EpisodeRegistry` | one `std::mutex` | observation, advance, reads (copies out) |
| `Runtime` metrics | one `std::mutex` | counter updates only |
| `Runtime` topology pointer | one `std::mutex` | shared pointer read/replace only |
| `Runtime::WorkerPool` | one `std::mutex` | queue push/pop only |

Rules that the implementation follows and the audit verified:

1. **No nested locks.** The runtime never holds two component locks at once. `Runtime::metrics()`
   reads each component first and merges afterwards, so the metrics lock is never taken under a
   component lock.
2. **No user code under a lock.** Callbacks and worker tasks run with the pool lock released.
3. **No callback can re-enter a locked component.** Classification, localization and correlation
   are pure functions over copies; the runtime hands them value snapshots.
4. **The worker pool is not reentrant.** A task never submits work to the pool it runs on, so a
   pool task can never wait for itself.
5. **Shutdown is a join, not a sleep.** `Runtime::stop_workers()` stops intake, drains the queue
   and joins every worker.

## Bounded resources

Every growth vector is bounded by `Limits` and enforced at the point of growth:

* evidence: `max_retained_evidence`, `max_evidence_per_subject`, `max_sources`;
* topology: per collection and per node;
* analysis: window records, citations, explanation steps, graph nodes/edges, localization depth;
* episodes: count, transitions, assessments, correlation group size and count;
* runtime: worker threads, pending tasks, ingest queue depth;
* persistence: snapshot bytes, sections, evidence, episodes;
* documents: JSON depth, node count, byte size; wire frames: byte size.

Externally derived sizes pass through the checked arithmetic helpers in `core/checked.hpp`; an
overflow is a reported error, never a silent wrap that could defeat a bound check.

## Errors

Failures are values (`Result<T>` / `Status`), not exceptions. Codes are stable identifiers
(`congestion::ErrorCode`) used by the CLI, the transport and the tests. Fence rejections are not
errors: they are outcomes that report which class of replay or staleness was refused.
