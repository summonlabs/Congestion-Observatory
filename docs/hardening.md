# Hardening log

Every item below was found by a suite (or by an audit) and fixed, then re-verified by rerunning the
complete suite in Release, Debug and AddressSanitizer configurations.

| # | Defect | Detected by | Fix |
|---|---|---|---|
| 1 | `assess_agreement` indexed `sources_per_cluster` with the cluster *count* instead of the cluster index, writing out of bounds and corrupting the heap | `assess.classification_is_deterministic` (crash), then AddessSanitizer | index the cluster explicitly during clustering; count each source once, in the first cluster it appears in |
| 2 | Fencing reported a stale generation for a stale *revision*, and a generation advance for a revision-only advance, so a replay could look like progress | `model.fence_rejects_every_replay_class` | report the most specific older component; revision-only advances go through the sequence check |
| 3 | `Topology::links_of_node` returned nothing: the adjacency index was keyed by endpoint *port* instead of endpoint *node* | `model.topology_builds_and_indexes` | resolve port to node while building the index |
| 4 | The localizer walked *out*-edges from the symptom, so it could never find a cause | `localize_episode.localization_finds_the_evidence_backed_cause` | walk in-edges (cause to symptom) and take the edge source as the next node |
| 5 | `Runtime::localize` queried evidence for the symptom only, so candidates had no data to be scored on | `runtime.ingest_classify_localize_end_to_end` | query the whole window; the classifier still filters by subject |
| 6 | Consecutive hops of a path had no causal relation, so link-to-link localization was impossible | same as 5 | add evidence-backed `topology-path-upstream` edges between consecutive hops |
| 7 | A queue occupancy of 10% produced a `pressure_observed` verdict | `assess.utilization_alone_is_never_congestion` | a backlog verdict requires occupancy above the policy ratio, or occupancy the source could not normalise; otherwise a `backlog_below_policy_threshold` blocker is reported |
| 8 | `JsonObject::set` scanned members linearly, making a document with many keys quadratic: 200k keys never finished parsing | `adversarial.hostile_json_documents_are_refused` (hang under ASan) | index members in an ordered map with a transparent comparator; strict parsing uses a single `try_insert` |
| 9 | Snapshot decoding required a non-empty tenant, so an unattributed episode made the whole snapshot unloadable | `restart.history_survives_and_liveness_does_not` | an empty tenant decodes to "unattributed" |
| 10 | Evidence labels and metadata were written by the encoder but never read back: silent data loss across a restart | audit of `snapshot.cpp` | encode and decode both, with bounds checked before allocation |
| 11 | `encode_snapshot` trusted caller supplied digests, so a snapshot could describe a different payload than it contained | `persistence.snapshot_bounds_are_enforced` | the encoder stamps the limits and topology digests it can derive itself |
| 12 | `EvidenceStore::sweep` checked cancellation only between removals, so an empty store ignored cancellation | `evidence.store_sweep_is_bounded_and_cancellable` | check the token before doing any work |
| 13 | A default constructed `EvidenceQuery` silently matched nothing because its window was `[0,0]` | `evidence.recovered_records_are_never_live` | a zero window means "the whole retention horizon" |
| 14 | `SnapshotStore::save` and `load` were not serialised, so concurrent callers could interleave the rotation | hardening audit | one mutex around both operations |
| 15 | The server served connections sequentially, so one silent peer stalled every other collector | hardening audit | bounded connection workers with a polite refusal above `max_connections` |
| 16 | The test framework lost an entire suite when a test threw, hiding which test failed | persistence/concurrency suites aborting with no output | the runner catches exceptions per test and reports them as failures |
| 17 | Several tests dereferenced `Result::value()` after a failed expectation, turning a failed check into an aborting exception | same as 16 | `CO_REQUIRE` / `CO_REQUIRE_OK` stop the test before any dereference |
| 18 | Test commands were passed through `cmd.exe`, whose quoting rules corrupted the trailing argument and silently produced empty output files | `endtoend.full_pipeline_from_the_command_line` | the CLI gained `--out`, and tests launch it as a real child process with no shell |
| 19 | The server's `protocol_errors` counter was declared, reported and never incremented, so protocol abuse was invisible in the statistics | `hardening.hostile_peer_cannot_stall_the_server` | every protocol rejection now increments the counter through the single `fail` path |
| 20 | `Runtime::load()` failed entirely when a snapshot contained an episode older than the live one, so a concurrent save/load cycle could abort a load | `hardening.whole_public_surface_under_concurrency` | a stale episode is skipped and reported in the load notes; state never moves backwards |
| 21 | The benchmark configured limits that failed validation, and configured its persistent runtime from a moved-from config | benchmark runs | the benchmark validates and prepares every configuration before moving it, and prints the error it hit |
| 22 | The CLI benchmark classified subjects that were not in any topology | CLI `bench` | the command admits a minimal two switch fabric first |

## Audits

**Deadlock and lock reentrancy.** The lock graph is documented in `architecture.md`. No path takes
two component locks; `Runtime::metrics()` reads components before taking the metrics lock; worker
tasks run with the pool lock released and never submit to their own pool; `stop_workers()` drains
and joins. The concurrency and hardening suites exercise ingest, classify, localize, evaluate,
correlate, explain, export, save, load, metrics and episode listing from up to seven threads at
once; they complete without a hang and with consistent counters.

**Arithmetic.** All externally derived sizes go through `checked_add` / `checked_mul` or
`checked_add_within`; overflow is a reported error. The core suite covers the overflow paths for
unsigned and signed addition and multiplication, and the timestamp conversions.

**Bounds.** Every collection has a configured bound and an adversarial test that drives past it:
sources, retained evidence per subject and in total, topology collections and per node ports, JSON
depth and node count, document bytes, frame bytes, graph nodes and edges, localization depth,
citations, explanation steps, episodes, transitions, assessments, groups, group members, snapshot
bytes, sections, evidence and episodes.

**Fuzzing.** The snapshot decoder is fuzzed with 400 seeded mutations plus systematic truncation at
every seventh byte; every outcome is either a clean decode with verified identities or a reported
error code. The frame decoder is fed hostile streams (zeros, oversized feeds, lying length fields,
corrupted checksums, bad magic).