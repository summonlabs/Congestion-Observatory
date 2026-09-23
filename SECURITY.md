# Security

## Reporting

Report suspected vulnerabilities privately to the maintainers through the repository's security
advisory channel. Please include a reproduction, the affected version and the configuration.

## Design notes relevant to security

* **No telemetry transmission.** The runtime never contacts an external service. The only network
  activity is the transport the operator starts, bound to loopback by default.
* **No hardware or privileged access.** The library opens no device, no driver and no privileged
  API.
* **Bounded input.** Frames, documents, snapshots, topology collections, evidence counts, episodes
  and analysis fan-out are all bounded, and externally derived sizes use checked arithmetic.
* **Integrity, not authenticity.** Snapshots are CRC-64 checked, which detects corruption, not
  tampering. Identity digests are not cryptographic. A snapshot from an untrusted source must be
  treated as untrusted input: the loader validates structure, bounds and identities, but it cannot
  prove authorship.
* **Authority is declared, not verified.** A source's authority level is caller-supplied. Deploy
  the transport behind a trusted boundary; do not expose the ingestion port to untrusted networks.
* **Denial of service.** The server bounds concurrent connections and per-frame size, and uses
  operation deadlines so a silent peer cannot stall it. Analysis and persistence are bounded per
  call. An operator can still exhaust memory by ingesting up to the configured retention bound;
  choose `Limits` for the host.
