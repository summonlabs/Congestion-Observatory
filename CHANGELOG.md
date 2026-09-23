# Changelog

All notable changes to Congestion Observatory are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026

First release.

### Added

* Typed identities for nodes, ports, links, queues, buffers, paths, flows, tenants, classes,
  sources, topologies, evidence, episodes and correlation groups.
* Evidence model with explicit provenance, clock domain, completeness, support, freshness and
  agreement, and a bounded store with generation/incarnation/sequence fencing and retirement.
* Classification into eight verdicts with citations, confidence basis, blockers and a deterministic
  rule-by-rule explanation, driven by a versioned and digested policy.
* Evidence-backed causal graph and localizer with ranked candidates, ambiguity reporting and depth
  bounds; edges without citations are refused.
* Episode identity, lifecycle, bounded history that reports dropped entries, and deterministic
  correlation grouping with recorded merge and rejection decisions.
* Versioned CRC-64 snapshot format with primary/fallback rotation, identity re-verification and
  conservative recovery that never applies a partial snapshot.
* Runtime facade with bounded worker pool, real cancellation, topology admission and metrics.
* Bounded frame protocol over loopback TCP with a server, a synchronous client and connection
  limits.
* `congestion-observatory` command line tool: ingest, classify, localize, episodes, episode,
  advance, explain, export, save, load, serve, push, selfcheck, bench.
* CMake package export with an independent `find_package` proof, three worked examples and a
  benchmark that verifies completed work.
* 129 tests across 14 suites: unit, integration, end to end, property/invariant, seeded randomized,
  adversarial, concurrency, restart/recovery, cross-process transport and hardening.

### Notes

* No congestion control, no hardware integration, no telemetry transmission.
* Windows x64 with MSVC is the validated platform; other platforms are configured but unproven.
