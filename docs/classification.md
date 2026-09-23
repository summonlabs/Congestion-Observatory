# Classification

`classify_subject` is a pure function of an evidence window and a versioned policy. The same
window and policy always produce the same verdict, features, citations, confidence and explanation.

## Verdicts

| Verdict | Meaning |
|---|---|
| `no_evidence` | nothing was supplied for this subject |
| `idle` | capacity available, demand low |
| `utilized_healthy` | high utilization, **no impairment evidence**: explicitly not congestion |
| `contention_observed` | several consumers on a loaded resource, no harm demonstrated |
| `pressure_observed` | a backlog above the policy ratio, no harm demonstrated |
| `saturated` | offered demand meets or exceeds serviceable capacity, no harm demonstrated |
| `congestion_confirmed` | fresh impairment evidence above threshold |
| `indeterminate` | evidence is stale, conflicting, incomplete, unsupported, mismatched or unknown |

`is_congestion_assertion(verdict)` is true only for `congestion_confirmed`. Every other verdict
is a *positive finding about the absence of proof*, and each carries blockers that say why.

## Rule order

```
R00 no records                    -> no_evidence
R01 generation mismatch on all    -> indeterminate (generation_mismatch)
R02 unsupported on all            -> indeterminate (evidence_unsupported_for_subject)
R03 no fresh deciding evidence    -> indeterminate (stale pressure / stale impairment / unknown)
R04 fresh impairment decisive     -> congestion_confirmed
R04 fresh impairment not decisive -> continue, with a blocker naming why
R05 demand >= capacity            -> saturated
R06 backlog above policy ratio    -> pressure_observed
R07 contention on a loaded link   -> contention_observed
R08 utilization above high        -> utilized_healthy (utilization_alone_is_not_congestion)
R09 otherwise                     -> idle
```

Post-processing always adds: conflicting source blockers, the single source blocker, incomplete,
unknown freshness, recovered and retired evidence blockers, and the missing demand context blocker.

## Impairment thresholds

A drop, mark, pause or retransmit record is decisive when it expresses a ratio at or above the
policy threshold; a positive *delta* counter of a counting unit is also decisive, because a delta
counter states what happened in this window. A **cumulative** counter never is: a lifetime total
cannot prove present harm, and the assessment says so explicitly.

Latency inflation needs a baseline: with fewer than two fresh samples no factor can be computed,
and a single sample never confirms congestion.

## Confidence

Confidence is an integer 0..100 derived from named contributions (deciding records, corroborating
sources, agreement, freshness, conflict, completeness) and reported together with its basis, so a
low number is explainable rather than mysterious. For `indeterminate` the number expresses how
strongly the evidence supports *not deciding*.

## Determinism

Everything that affects a verdict is either data or policy:

* records are ordered by (observation time, insertion index, id);
* agreement clustering runs in ascending value order with a fixed tolerance;
* confidence uses integer arithmetic;
* the explanation is a list of named rules in fixed order;
* `FeatureSnapshot::feature_digest` is a digest of the extracted features, and the property suite
  asserts it is identical for repeated and permuted inputs.

The policy version and digest are part of every explanation and every persisted snapshot: a policy
change is visible, never silent.
