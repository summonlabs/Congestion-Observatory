# REAL, SYNTHETIC and UNSUPPORTED proof surfaces

This project is explicitly honest about what has been proven and what has not.

## REAL

*Product functionality that is implemented and exercised, in the configuration that was built:*

* Evidence ingestion with fencing against stale epoch, generation, revision, incarnation and
  sequence replays, plus authority floors — real code, real tests.
* Freshness, agreement (including minorities and incomparability), completeness, support and
  unknown states as distinct, separately tested outcomes.
* Classification into eight verdicts with explicit blockers, citations, confidence basis and a
  deterministic explanation.
* Evidence-backed causal localization with ambiguity reporting, depth bounds and rejection of
  structural-only paths.
* Episode identity, lifecycle, bounded history, deterministic correlation grouping with recorded
  merge decisions.
* Versioned, CRC-checked snapshots with conservative recovery, primary/fallback rotation and
  identity re-verification on load.
* A loopback TCP transport with a bounded frame protocol, a bounded connection model and a polite
  refusal above the connection limit.
* A command line tool covering ingest, classify, localize, episodes, episode, advance, explain,
  export, save, load, serve, push, selfcheck and bench.
* A CMake package that installs and is consumed by an independent `find_package` project.
* AddressSanitizer builds and runs the full suite (memory errors). **Leak detection is not
  claimed**: MSVC's AddressSanitizer on Windows does not provide LeakSanitizer, so no leak proof
  is offered.
* Independent real process transport: the test suite spawns the tool as a separate operating system
  process as both the client and the server of a real loopback TCP connection.

## SYNTHETIC

*Test inputs and scenarios, clearly not real fabric traffic:*

* Every topology in the tests and examples (`host1 -> leaf1 -> leaf2 -> host2` and the small
  chains used in the localization tests) is a hand-written synthetic fabric.
* All evidence documents, counter values, utilization ratios, drop rates and timestamps in the test
  suites are synthetic and deterministic; no production telemetry was used.
* The benchmarks run against synthetic topologies and synthetic record streams.
* The CLI end-to-end scenario uses synthetic documents written by the test itself.

## UNSUPPORTED

*Explicitly out of scope or not proven. None of the following is claimed:*

* **No switch, ASIC, RDMA, InfiniBand, NVLink, multi-host or vendor telemetry integration exists.**
  There is no device driver, no vendor SDK, no hardware counter reader and no on-switch agent in
  this repository. The runtime consumes documents and frames that a collector would produce; it
  never talks to a device.
* No congestion *control*: no rate limiting, no rerouting, no queue programming, no marking, no
  pacing, no drop policy. The runtime is an observer by construction: the library links no
  privileged API and exposes no actuator.
* No distributed consensus or multi-node aggregation: a single process owns its state. Multiple
  collectors can push to one runtime over the transport, but the runtime itself is not replicated.
* No latency measurement of real traffic: latency inflation can only be derived from samples a
  source supplies.
* No cryptographic authentication of sources: authority is an explicit, caller-supplied level, and
  identity digests are not cryptographic.
* No hard real-time guarantees: analysis is bounded but not scheduled.
* Platform validation: built and tested on Windows x64 with MSVC 19.44 (/W4 /WX), Release, Debug and
  AddressSanitizer. The build configures GCC/Clang warning sets and POSIX sockets, but **no Linux or
  macOS build was executed here**, so portability is designed-for, not proven.
* No performance claim against other systems: benchmarks measure completed work in this
  implementation only.
