# Evidence model

Every observation answers: **what** was observed, **by which source**, **for which generation**, at
what **observation and receive time**, under which **source incarnation**, and whether the evidence
is fresh, stale, conflicting, incomplete, unsupported or unknown.

## Record

```
EvidenceRecord
  id            deterministic identity (see identity.md)
  kind          20 kinds, each mapped to exactly one argumentative role
  subject       node | port | link | queue | buffer | path | flow | tenant | class | source | topology
  provenance    source, authority level, transport, collector
  fence         source, boot id, incarnation, epoch/generation/revision, sequence
  observed_at   source clock reading
  received_at   collector clock reading
  clock         clock domain (never compared across domains)
  value         scalar + unit + semantics (gauge, cumulative counter, delta counter, rate)
  validity      how long this observation may be used
  completeness  complete | partial | incomplete | unknown
  support       supported | unsupported | unknown
  labels        bounded, canonical names
  metadata      bounded key/value pairs
  note          bounded free text
  recovered_from_snapshot / retired   liveness provenance (see persistence.md)
```

## Roles: the boundary in one table

| Role | Kinds | What it can prove |
|---|---|---|
| utilization | link/port utilization | that capacity is being consumed |
| contention | active flow/class count | that several consumers share a resource |
| pressure | queue occupancy, queue depth, buffer occupancy | that a backlog exists |
| impairment | drops, marks, pauses, latency, retransmits | that traffic was actually harmed |
| demand | offered demand, achieved throughput | what load was offered and served |
| capacity | capacity advertisement | the denominator for utilization and demand |
| topology | oper state, topology advertisement | that a structure exists |
| liveness | source heartbeat | that a source is alive |

**Utilization is never congestion.** It is an input to interpretation. A saturated resource with no
impairment is reported as `saturated`, a busy healthy resource as `utilized_healthy`, and neither
message asserts congestion.

**Impairment is the only role that can assert congestion**, and only when it is fresh, supported,
generation matched, and above the configured threshold (or an unquantified positive delta counter).

## Freshness

`assess_freshness` classifies a record at an explicit instant:

* `fresh` – usable as deciding evidence;
* `aging` – usable as corroboration only;
* `stale` – cannot prove anything about the present;
* `expired` – beyond the policy window or the record's own validity;
* `unknown` – missing timestamps, unknown clock domain, implausible ordering, or a record that was
  recovered from a snapshot or retired by a newer incarnation.

Absence of evidence is never positive evidence: a subject with no records yields `no_evidence`,
not `idle`.

## Agreement

`assess_agreement` compares the records of one subject and kind inside one window:

* `single_source` – nothing corroborates *or* contradicts (explicitly weaker than agreement);
* `unanimous` – one cluster of sources;
* `minority` – a majority cluster plus a dissenting minority; both stay cited;
* `conflicting` – no position holds a majority; every position stays cited;
* `incomparable` – units differ, or two cumulative counters with different epochs.

Values are clustered in ascending order with a relative tolerance, and each source counts once
towards the cluster it first appears in, so a chatty source cannot manufacture a majority.
