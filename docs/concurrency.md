# Concurrency and lock audit

## Threads

* The caller's threads: ingestion, analysis, persistence.
* The runtime's worker pool (`Runtime::start_workers()`): bounded by `limits.worker_threads`
  (0 means no background threads at all, the default for tests) and by `max_pending_tasks`.
* The transport server: up to `ServerConfig::max_connections` connection threads, joined by
  `run()`.

## Lock graph

```
EvidenceStore::mutex_      ─┐
EpisodeRegistry::mutex_    ─┼─ never held together
Runtime::metrics_mutex_    ─┤
Runtime::config_mutex_     ─┤
WorkerPool::mutex          ─┘
SnapshotStore::mutex_      ── held only around file operations, never around a component call
```

The audit checked every path that touches more than one component:

| Path | Behaviour |
|---|---|
| `Runtime::ingest` | takes the store lock, releases it, then takes the metrics lock |
| `Runtime::classify_with` | copies the topology shared pointer, queries the store (lock taken and released), classifies outside every lock, then takes the metrics lock |
| `Runtime::localize` | same, plus the localization which is pure |
| `Runtime::evaluate` | classification, then the registry (its own lock), then metrics |
| `Runtime::correlate` | `registry_->all()` copies under the registry lock, correlation runs unlocked |
| `Runtime::metrics` | reads each component first, then takes the metrics lock to merge |
| `Runtime::save` | topology pointer, `registry_->all()`, `store_->query`, then the snapshot store lock |
| `Runtime::load` | snapshot store lock, then topology pointer, registry and store calls |
| `WorkerPool` task | runs with the pool lock released and never submits to its own pool |
| `IngestServer` connection | uses its own stats lock; the runtime is called unlocked |

No path takes two component locks. No user code runs while a component lock is held. No callback
re-enters a locked component, because every analysis entry point receives value copies.

## Shutdown

`Runtime::request_shutdown()` latches cancellation and marks the runtime as shutting down; further
ingestion returns `shutting_down`. `Runtime::stop_workers()` sets the pool's stop flag, wakes every
worker, lets them drain the queue and joins them. Tasks already queued are executed, never
abandoned: the hardening suite asserts `tasks_completed == tasks_submitted`.

`IngestServer::stop()` closes the listener (releasing a thread blocked in `accept`, which is a
transport-level deadline, not a test timeout) and `run()` joins every connection worker before
returning.

## What is deliberately not concurrent

* `SnapshotStore` serialises `save` and `load` with one mutex: two threads may call them
  concurrently, but the file rotation is atomic with respect to other store operations.
* Configuration changes (`admit_topology`) swap a shared pointer under a mutex; analyses hold the
  pointer for their duration, so a topology is never observed half-replaced. Analyses that started
  under the previous generation finish against it.
* The runtime does not replicate state across processes. Two runtimes sharing a state directory
  would fight over the snapshot files; that is out of scope and not claimed.
