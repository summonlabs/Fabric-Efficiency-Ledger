# Fabric Efficiency Ledger - design

Copyright 2026 Summon Software Labs. Licensed under the Apache License 2.0.

This document describes the architecture, the invariants the runtime maintains,
and the derivation pipeline that produces every published number.

## 1. Layers

`@
tools/fel            command line tooling (init, ingest, close, correct, query,
                     aggregate, reconcile, explain, export, verify, stats,
                     compact, lock-hold)
examples/            three buildable walkthroughs
benchmarks/          four benchmarks that measure completed work
include/fel/         the public headers; everything a consumer needs
src/                 the implementation
tests/               unit, integration, property, adversarial, concurrency,
                     end-to-end and independent-process suites
tests/downstream/    an independent CMake project that consumes the installed
                     package through find_package
`@

The public surface is the `fel` namespace. Internal helpers live in
`fel::detail` (document codecs) or in anonymous namespaces.

## 2. Types

| Header | Content |
| --- | --- |
| `fel/error.hpp` | the closed error taxonomy and `Result<T>` |
| `fel/checked.hpp`, `fel/limits.hpp` | checked arithmetic and every hard bound |
| `fel/crypto.hpp` | in-tree SHA-256 (FIPS 180-4) and CRC-32C, no external dependencies |
| `fel/id.hpp` | content-derived strongly typed identities |
| `fel/time.hpp` | integer nanosecond instants, durations, RFC 3339 |
| `fel/measure.hpp` | measure kinds, categories, coverage, efficiency summaries |
| `fel/topology.hpp` | sources, incarnations, generations, bindings, scopes, resources |
| `fel/observation.hpp` | evidence records and ingest reports |
| `fel/policy.hpp` | the deterministic accounting policy |
| `fel/freshness.hpp` | freshness and admissibility assessment |
| `fel/claim.hpp` | accounting cells, claims, conflicts |
| `fel/period.hpp` | periods, revisions, conservation domains, corrections |
| `fel/aggregate.hpp` | aggregation axes and bounded buckets |
| `fel/persistence.hpp` | segments, manifest, recovery, compaction, locking |
| `fel/concurrency.hpp` | bounded queue, thread pool, lock audit |
| `fel/serialize.hpp` | canonical JSON, CSV and field framing |
| `fel/io.hpp` | evidence and topology document codecs, bounded file helpers |
| `fel/ledger.hpp` | the runtime itself |
| `fel/provenance.hpp` | the provenance stamp carried by every result |

### Strong typing

Identities are 128-bit values derived by hashing a tag name and a
length-prefixed field list, wrapped in a template parameterised by a tag type.
A `ResourceId` cannot be passed where a `ScopeId` is expected, and the same
textual parts in two domains cannot collide because the tag participates in the
hash. Epochs, sequences, revision ordinals, boot epochs and authority ranks are
separate types for the same reason.

Quantities always carry their unit. There is no naked integer anywhere on the
accounting path.

## 3. The derivation pipeline

`Ledger::derive` is a pure function of `(policy, topology, period, retained
evidence, as_of)`. It performs the following steps, in order, over an ordered
set:

1. **Membership.** An observation is a candidate if its reporting window is
   fully contained in the period. Windows that straddle a boundary are contained
   in no period and are refused, which removes the only way a quantity could
   otherwise be counted in two periods.
2. **Deterministic ordering.** Candidates are sorted by evidence identity, so
   the pipeline never depends on arrival order.
3. **Identity collisions.** Records that share an `EvidenceId` but differ in
   content cannot both be real. Every participant is excluded and explained.
4. **Delivery-origin fencing.** Records that share an `origin` are grouped. An
   identical group is one physical measurement and is counted once; a divergent
   group is excluded in full and recorded as a conflict.
5. **Epoch fencing.** Per source incarnation, the peak observed epoch is
   computed over the whole candidate set, and any record below it is fenced.
6. **Admissibility.** Provenance class, incarnation coverage, epoch floor,
   generation binding and freshness are each assessed and reported separately.
   Every rejection names a specific reason.
7. **Correlation.** Each admissible contribution is attributed to exactly one
   `ClaimKey` through the topology: resource, else path, else flow, else
   reservation, else unattributed.
8. **Dispute resolution.** Contributions are grouped by `(cell, window)`. One
   agreed amount is counted once. Disagreement is resolved only if the policy
   says how, and the resolution is recorded either way.
9. **Folding.** Cells accumulate into `Claim` records carrying known totals,
   contribution counts, unmeasured counts, evidence identities, sources and
   provenance classes.
10. **Conservation.** Domains are formed per `(scope, generation, measure
    kind)`. Attributed totals are compared with independently reported totals.
    Residuals and excesses are reported, never netted.
11. **Efficiency.** Per measure kind, the usefulness partition is summed. A
    ratio is published only when every contribution was measured, nothing landed
    in the unknown bucket, and no domain for that measure is inconsistent.
12. **Proof surfaces.** The set of provenance classes that contributed is
    attached to the revision and to every result derived from it.

Nothing in this pipeline reads a clock, a random source, a hash-map iteration
order or a thread identifier.

## 4. Invariants

The property suite checks these on seeded randomized evidence:

* `claim.identity == claim.key.identity()` for every claim.
* Claims are strictly ordered and unique per key.
* Every unknown entry carries a reason other than `none` and a positive
  evidence count.
* `residual` implies `attributed + residual == observed`; `consistent`
  implies `attributed == observed`; `unverifiable` implies no observed total.
* `determinate` efficiency implies a positive denominator, a positive reduced
  denominator, a permille value at most 1000, and `useful <= denominator`.
* `denominator == useful + necessary + avoidable + failed`.
* `evidence_admitted + evidence_rejected == evidence_considered`.
* The sum of conservation domain totals equals the sum over claims, because the
  domains partition the same contributions.

## 5. Persistence layout

`@
<store>/
  fel.lock                 exclusive advisory lock, held for the process lifetime
  fel.manifest.json        canonical JSON with a self-describing integrity field
  seg-00000001.felseg      append-only segment
`@

Segment format:

`@
magic "FELSEG01" | u32 format version | u64 segment id | i64 created | u32 reserved   (32 bytes)
repeat:
  u32 payload length | u32 CRC-32C(payload) | payload
  payload = u8 record kind | u64 ordinal | i64 written_at | payload bytes
footer:
  magic "FELFOOT1" | u64 record count | 32-byte SHA-256 over every frame | u32 CRC-32C
`@

A segment is sealed, with its footer, at the end of every commit. The manifest
records the sealed digest and byte length of each segment, and its own integrity
field is a SHA-256 over the document with that field removed. The manifest is
published by writing a temporary file, flushing it and renaming it over the
target.

On open, every segment is verified against the manifest. A segment that ends in
the middle of a record, fails a frame checksum, or disagrees with the manifest is
reported and the longest valid prefix is recovered. Nothing is rewritten, and
`RecoveryPolicy::Strict` turns any damage into a failed open.

Compaction streams the surviving records into fresh segments, publishes the new
manifest, and only then removes the old files. A crash before the manifest is
published leaves orphaned segments, which are swept on the next open; a crash
after it leaves the store consistent.

## 6. Locking discipline

One mutex, `Ledger::Impl::state`, guards all mutable ledger state. It is a
`CheckedMutex` with the `StoreState` rank. Every acquisition is audited:

* reentrant acquisition sets `Guard::reentrant()` and increments
  `lock_audit().reentrancy_detections`;
* acquiring a mutex ranked outside one already held sets
  `Guard::order_violation()` and increments
  `lock_audit().lock_order_violations`.

Callers check the guard immediately and refuse the operation if either flag is
set, so a defect surfaces as an error rather than a hang. The audit uses only
atomics and thread-local storage, so it can never itself deadlock.

The public API partitions into a small number of entry points that acquire the
lock exactly once. `Ledger::derive_locked` exists specifically so that
`close_period` and `correct_period` can derive while already holding the
lock without re-entering it.

Cross-process exclusion uses a separate mechanism: an OS-level exclusive lock on
`fel.lock`, held for the lifetime of the store object, with a bounded wait
configured by `lock_timeout_ms`.

## 7. Concurrency

* `BoundedQueue<T>` blocks producers when full and is closed or aborted on
  shutdown, so a producer that outruns the consumers cannot grow memory without
  limit.
* `ThreadPool` runs a fixed number of workers over a bounded queue, counts
  submissions, completions, failures, cancellations and rejections, and joins
  every worker on shutdown. Cancellation is a real token that jobs observe.
* `Ledger::ingest` validates and commits under the state lock.
  `Ledger::ingest_parallel` spreads shape validation and JSON encoding across
  workers and then commits through the identical sequential path, so
  concurrency cannot change a total. The property suite compares the two.

## 8. Serialization and determinism

`FieldWriter` produces both a canonical text form and a SHA-256 over the same
fields with explicit length framing, so the text and the digest cannot disagree.
The digest is finalised once and cached, which makes repeated calls agree.

`Json` stores objects in a sorted map, so serialization is canonical: the same
logical document always produces byte-identical output. Integers are stored
exactly, including the full unsigned 64-bit range. The parser is strict and
bounded: duplicate keys, trailing content, raw control characters, excessive
depth and excessive token counts are all rejected.

## 9. Test strategy

| Layer | Approach |
| --- | --- |
| unit | known-answer vectors, boundary values, malformed input |
| integration | complete lifecycles against an in-memory or persisted ledger |
| property | seeded randomized evidence checked against the invariants above |
| adversarial | replay, forgery, contradiction, clock abuse, overflow, collision |
| concurrency | concurrent ingest against a sequential baseline, readers under load, cancellation |
| end-to-end | ingest to close to query to export |
| process | the CLI as a real separate process, proving cross-process locking |

No suite uses a timeout. Synchronisation is by data dependency, process exit or
a blocking pipe read. The independent-process test synchronises on a line the
child writes to stderr after it genuinely holds the lock.
