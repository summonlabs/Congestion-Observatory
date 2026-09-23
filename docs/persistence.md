# Persistence and restart

## Format

```
magic "COSNAP01" | format major u32 | format minor u32
limits digest u64 | policy digest u64 | topology digest u64 | created_at i64
section count u32 | payload bytes u64 | payload CRC-64 u64
section table: id u32, offset u64, length u64, CRC-64 u64 (repeated)
payload bytes
```

All integers are little endian and written explicitly, so the format does not depend on host
endianness. CRC-64/XZ protects the payload and every section independently.

`encode_snapshot` stamps the limits digest and the topology digest itself; the policy digest is
supplied by the caller that owns the policy. A caller therefore cannot desynchronise a header from
the payload it describes.

## Conservative recovery

`decode_snapshot` is all-or-nothing:

1. magic, format major, format minor (a newer minor is refused, not guessed);
2. limits digest and policy digest must match the running configuration;
3. payload length must equal the file size exactly (trailing bytes are corruption);
4. payload CRC, then every section CRC;
5. section bounds are checked with overflow-safe arithmetic before any allocation;
6. object counts are checked against the configured bounds before allocation;
7. episode and evidence identities are **recomputed** and compared with the stored ones.

Any failure yields an integrity or version error and **no state is applied**.

`SnapshotStore` writes through a temporary file and renames it into place, keeping the previous
snapshot as `<name>.snapshot.previous`. Loading tries the primary and then the fallback, reporting
every rejection in `LoadReport::notes`.

## Restart preserves history, not liveness

* Episodes keep their identity, revision, state and bounded history.
* Evidence is restored as `recovered_from_snapshot`; its freshness is `unknown` with the reason
  `persisted_dynamic_evidence_not_live`, and it can never support a current verdict.
* Every source is reset to not-live: `live_sources` is zero until the source speaks again.
* The first live observation after a restart is accepted as `accepted_first_observation`, because
  the process-local fence table is deliberately not persisted.
* Recovered records stay visible in exports and in the feature snapshot, flagged as history.

This is the behaviour the restart suite asserts: history survives, liveness does not, and a
classification over a restarted runtime reports `indeterminate` with the
`persisted_evidence_is_not_live` blocker until fresh evidence arrives.
