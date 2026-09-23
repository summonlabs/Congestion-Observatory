# Identity

Identity is textual, canonical and content addressed. Nothing is keyed by pointer, address, arrival
order or wall clock, so two processes that observe the same object agree on its identity.

## Entities

`NodeId`, `PortId`, `LinkId`, `QueueId`, `BufferId`, `PathId`, `FlowId`, `SourceId`,
`TenantId`, `ClassId`, `TopologyId` are distinct types over a canonical `Name`. Two different
kinds of object therefore cannot be silently interchanged or compared.

A canonical name is ASCII, 1..96 characters from `[A-Za-z0-9]` plus `- _ . : / @ # | + = ,`,
with no leading, trailing or doubled separator. Case is preserved and never normalised: `Leaf1`
and `leaf1` are different names.

## Derived identities

`EvidenceId`, `EpisodeId` and `GroupId` are 128-bit digests of a canonical byte encoding:

* **evidence**: source, boot, incarnation, epoch/generation/revision, sequence, kind, subject, unit,
  semantics and observation time — that is, the *address* of the observation, not the measured
  value. A corrected value for the same address keeps its identity, and the fence guarantees at
  most one accepted value per address (a second value with the same sequence is refused as a
  replay). `docs/evidence-model.md` states the consequence: identity is stable under
  representation changes and independent of floating point formatting.
* **episode**: scope, tenant, mechanism, generation vector and policy version. Deliberately *not*
  a function of wall clock: the same phenomenon seen by two collectors yields the same episode id,
  while a generation change yields a new identity so a stale replay cannot extend a retired
  episode.
* **group**: the sorted member episode ids, so the group id is a pure function of its membership.

The digest is FNV-1a based with an avalanche finaliser. It is an identity and integrity aid, **not**
a cryptographic hash, and it is never used to authenticate anything.

## Generation, incarnation and fencing

```
GenerationVector { epoch, generation, revision }
FenceVector      { source, boot, incarnation, generation, sequence }
```

`evaluate_fence` decides whether an observation may influence current state:

| Decision | Condition |
|---|---|
| `accepted_first_observation` | first record from this source in this process |
| `accepted_reboot` | higher boot id; previous incarnation retired, liveness reset |
| `accepted_epoch_advance` | higher epoch, generation not decreased |
| `accepted_generation_advance` | same epoch, higher generation |
| `accepted_in_order` / `accepted_gap_detected` | same generation and revision, sequence advances |
| `rejected_stale_epoch` / `stale_generation` / `stale_revision` | the most specific older component |
| `rejected_incomparable_generation` | higher epoch combined with a lower generation |
| `rejected_replayed_sequence` | sequence not greater than the last accepted one |
| `rejected_stale_incarnation` | lower boot id, or a lower incarnation |
| `rejected_inconsistent_boot` | incarnation advanced without a new boot id |
| `rejected_authority_too_low` | authority below the ingest policy minimum |
| `rejected_unknown_source` | no source identity |

When a reboot is accepted, every retained record from that source is marked `retired`: it stays as
history and can never contribute to a current verdict.
