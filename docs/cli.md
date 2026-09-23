# Command line reference

`congestion-observatory <command> [options]`, installed as `congestion-observatory`.

## Commands

| Command | Purpose |
|---|---|
| `version` | print the build identity |
| `limits` | print the resource bounds and their digest |
| `topology --file <json>` | validate a topology document and print its digest |
| `ingest --file <json> [--save]` | ingest an evidence document (accumulates over `--state`) |
| `classify --subject <kind:name> [--file <json>]` | classify a subject over a window |
| `localize --subject <kind:name> [--file <json>]` | localize an asserted congestion |
| `episodes [--json] [--open-only]` | list episodes |
| `episode --id <hex32>` | print one episode and its transitions |
| `advance --now <instant> [--save]` | apply resolution and expiry windows |
| `explain [--subject <kind:name> \| --id <hex32>]` | print the rule-by-rule reasoning |
| `export [--evidence] [--json]` | export the runtime state |
| `save --state <dir> [--file <json>]` | persist the state |
| `load --state <dir>` | restore the state and report the outcome |
| `serve --port <n> [--io-deadline <dur>]` | serve the ingestion protocol on loopback |
| `push --endpoint <host:port> --file <json>` | push records to a server |
| `selfcheck` | run the built-in invariant checks |
| `bench [--records <n>]` | measure completed ingestion and analysis work |

## Options

| Option | Meaning |
|---|---|
| `--state <dir>` | snapshot directory; enables persistence |
| `--state-name <name>` | snapshot base name (default `congestion-observatory`) |
| `--topology <file>` | topology document to admit |
| `--file <file>` | evidence document (ingest) or fresh observations (classify/localize/explain) |
| `--subject <kind:name>` | subject, e.g. `link:leaf1-leaf2`, `queue:leaf1-leaf2/q0` |
| `--window <dur>` | evaluation window, default `10s` (`ms`, `s`, `m`, `h`) |
| `--now <instant>` | evaluation instant: ISO-8601 UTC or `@epoch-seconds` |
| `--workers <n>` | background worker threads (0 = synchronous analysis) |
| `--policy-version <text>` | policy version label; a change makes old snapshots refuse to load |
| `--out <file>` | write output to a file instead of stdout |
| `--json` | machine readable output where supported |
| `--minimal-limits` | use the small bounds used by adversarial tests |
| `--io-deadline <dur>` | transport operation deadline (default `5s`) |

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success; for `classify`/`localize`, congestion asserted or localized |
| 1 | success without an assertion (not congested, unlocalized); or a usage error |
| 2 | runtime error (the reason is printed to stderr with its error code) |
| 3 | partial success: some records were fenced or rejected, or a selfcheck invariant failed |

## Topology document

```json
{
  "generation": {"epoch": 1, "generation": 1, "revision": 1},
  "revision_label": "fabric-2026-01",
  "nodes":  [{"id": "leaf1", "kind": "switch", "labels": ["pod-a"]}],
  "ports":  [{"id": "leaf1/1", "node": "leaf1", "speed_bps": 100000000000}],
  "links":  [{"id": "leaf1-leaf2", "endpoint_a": "leaf1/2", "endpoint_b": "leaf2/1",
              "capacity_bps": 100000000000}],
  "queues": [{"id": "leaf1-leaf2/q0", "link": "leaf1-leaf2", "index": 0,
              "min_share_bp": 0, "max_share_bp": 10000}],
  "buffers":[{"id": "leaf1/2/buf0", "port": "leaf1/2", "pool": 0, "cells": 4096}],
  "paths":  [{"id": "host1-host2", "hops": ["host1-leaf1", "leaf1-leaf2", "leaf2-host2"]}],
  "flows":  [{"id": "flow-a", "path": "host1-host2", "tenant": "tenant-a", "class": "class-1"}]
}
```

Every reference is validated: an unknown node, port, link or path is an error, not a silent default.
`generation` may also be written as the string `"epoch/generation/revision"`.

## Evidence document

Either a bare array of records or an object with a `records` array:

```json
{"records": [{
  "kind": "drop_count",
  "subject": "link:leaf1-leaf2",
  "source": "collector-1",
  "authority": "measured",
  "transport": "agent",
  "collector": "collector-1",
  "boot": 1, "incarnation": 1,
  "epoch": 1, "generation": 1, "revision": 1, "sequence": 42,
  "observed_at": "2026-01-01T00:00:00.000000000Z",
  "received_at": "2026-01-01T00:00:00.100000000Z",
  "clock": "collector_wall_clock",
  "value": 0.0004,
  "unit": "ratio",
  "semantics": "rate",
  "validity_ns": 5000000000,
  "completeness": "complete",
  "support": "supported",
  "labels": ["prod"],
  "metadata": {"agent": "v1"},
  "note": "counted on ingress"
}]}
```

Kinds: `link_utilization`, `port_utilization`, `queue_occupancy`, `queue_depth`,
`buffer_occupancy`, `drop_count`, `mark_count`, `pause_count`, `pause_duration`,
`latency_sample`, `offered_demand`, `achieved_throughput`, `active_flow_count`,
`active_class_count`, `oper_state`, `source_heartbeat`, `topology_advertisement`,
`capacity_advertisement`, `retransmit_count`.

Units: `ratio`, `cells`, `packets`, `bytes`, `nanoseconds`, `bps`, `count`, `boolean`,
`frames`. Semantics: `gauge`, `cumulative_counter`, `delta_counter`, `rate`.

Clock domains: `source_monotonic`, `source_wall_clock`, `collector_monotonic`,
`collector_wall_clock`. An unknown domain means unknown freshness — never fresh.

## Wire protocol

`magic u32 | version u32 | type u32 | flags u32 | body length u64 | payload | CRC-64 u64`

Types: `hello`, `welcome`, `ingest`, `ingest_ack`, `stats`, `stats_reply`, `ping`, `pong`,
`bye`, `error`. The client is strictly synchronous (one frame in flight), which keeps the
protocol and its tests deterministic. A declared length above the configured bound is refused before
any allocation, and every payload is checksummed.
