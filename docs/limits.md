# Resource bounds

Defaults, all overridable through `congestion::Limits`. `Limits::validate()` rejects internally
inconsistent combinations, and the digest of the bounds is part of a snapshot's identity: a
snapshot written under different limits is refused rather than truncated.

| Area | Bound | Default |
|---|---|---|
| ingestion | `max_sources` | 512 |
| | `max_retained_evidence` | 200000 |
| | `max_evidence_per_subject` | 8192 |
| | `max_metadata_entries` / key / value | 16 / 48 / 128 |
| | `max_note_bytes`, `max_labels` | 256, 16 |
| documents | `max_document_bytes` | 8 MiB |
| | `max_json_depth`, `max_json_nodes` | 32, 100000 |
| wire | `max_frame_bytes` | 1 MiB |
| topology | nodes / ports / links / queues | 4096 / 16384 / 16384 / 65536 |
| | buffers / paths / flows | 16384 / 16384 / 65536 |
| | `max_path_hops`, `max_ports_per_node` | 64, 512 |
| analysis | `max_window_records` | 8192 |
| | `max_citations`, `max_explanation_steps` | 64, 128 |
| | `max_result_candidates`, `max_localization_depth` | 16, 8 |
| | `max_graph_nodes`, `max_graph_edges` | 8192, 32768 |
| episodes | `max_episodes` | 4096 |
| | `max_episode_assessments`, `max_episode_transitions` | 64, 512 |
| | `max_groups`, `max_group_members` | 1024, 64 |
| runtime | `worker_threads` | 2 (0 = synchronous) |
| | `max_pending_tasks`, `max_ingest_queue_depth` | 1024, 4096 |
| persistence | `max_snapshot_bytes` | 64 MiB |
| | `max_snapshot_sections` | 32 |
| | `max_snapshot_evidence`, `max_snapshot_episodes` | 100000, 4096 |

## What happens at a bound

* **Evidence**: the oldest retained record is evicted, per subject first and then globally. The
  eviction count is reported in metrics and in the ingest outcome. Nothing is silently dropped.
* **Episodes**: a new episode may retire the oldest *terminal* episode. If every episode is still
  live the observation is refused with `limit_exceeded` and the episode counter records the drop,
  because displacing an open investigation would be worse than refusing new history.
* **Episode history**: the oldest transitions and assessments are dropped from the front and the
  counts of dropped entries are kept in the episode (`history_truncated`).
* **Sources**: a new source beyond `max_sources` is refused.
* **Snapshots**: encoding refuses to exceed `max_snapshot_bytes`; decoding refuses a snapshot whose
  declared payload exceeds it before allocating anything.
* **Frames**: the encoder refuses an oversized frame; the decoder refuses a declared length above
  the bound before allocating the body.
* **JSON**: depth, node count and document size are checked while parsing.
* **Correlation**: group membership and group count are capped; a capped group is flagged
  `size_capped` and episodes that fit nowhere stay ungrouped and unreported-as-grouped.

Sizes derived from external input are combined with checked arithmetic; an overflow returns
`arithmetic_overflow` instead of wrapping.
