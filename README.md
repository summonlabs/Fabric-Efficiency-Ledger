# Fabric Efficiency Ledger

Deterministic accounting of useful network work versus non-useful or avoidable
resource consumption.

The Fabric Efficiency Ledger (FEL) is a standalone, vendor-neutral C++20 runtime
that answers one question with evidence: *of the work a fabric actually did,
how much was useful, how much was avoidable, and how much can we not explain?*
It ingests typed telemetry observations, classifies them, fuses them into
immutable accounting cells, checks conservation against independently reported
totals, closes accounting periods, and explains every number it publishes.

Summon Software Labs Fabric OS runtime. Apache License 2.0. No telemetry
transmission.

---

## What the runtime does and does not do

**In scope** - this is what the code in this repository implements:

* Typed identities for ledgers, accounting periods, resources, flows, paths,
  reservations, sources, source incarnations, fabric generations, epochs and
  evidence.
* A closed category set: useful delivered work, retransmission, duplication,
  reroute overhead, idle reservation, failed transfer, stranded capacity,
  measurable control overhead, and an explicit unknown/unattributed bucket.
* Stable, content-derived accounting identities and double-count prevention at
  four independent levels (delivery-origin fencing, evidence-identity
  collision detection, category partition, scope partition).
* Conservation checks per `(scope, generation, measure kind)` domain, with
  residuals attributed to the explicit unknown bucket and over-attribution
  reported rather than silently netted off.
* Deterministic attribution and explanation: identical evidence and identical
  policy produce byte-identical totals, digests and exports.
* Generation-aware correlation, freshness classification, epoch and sequence
  fencing, and provenance stamping on every result.
* Hierarchical and time aggregation with bounded result sets.
* Immutable closed periods with linear correction lineage.
* Versioned, integrity-checked, crash-safe persistence with conservative
  recovery.
* `ingest`, `close`, `correct`, `query`, `aggregate`, `reconcile`,
  `explain`, `export`, `verify`, `stats` and `compact` tooling.

**Out of scope** - the runtime deliberately does none of these, and no code
path attempts them:

* It does not schedule traffic, program routes, enforce quotas or police
  admission control. It is a ledger, not a controller.
* It does not infer monetary cost. Cost requires explicit operator-supplied cost
  inputs, which this version does not consume.
* It does not invent efficiency from missing telemetry. A quantity that was not
  measured is *unknown*, never zero, and an unknown contribution always makes
  the affected efficiency ratio indeterminate.
* It does not claim distributed or clustered behaviour. One store has exactly
  one writer at a time, enforced by an OS-level exclusive lock that is proven
  across independent processes.
* It does not claim switch, ASIC, RDMA, InfiniBand, NVLink or multi-host
  behaviour. No hardware was involved in building or testing it.

---

## Build

Requirements: CMake 3.25 or newer, a C++20 compiler, and Ninja or another
generator. MSVC 19.4x is the toolchain used for the reference builds.

`@powershell
# Release build with the full test suite, tooling, examples and benchmarks
pwsh -File scripts/build.ps1 -Config Release -Build -Test

# Debug build as well
pwsh -File scripts/build.ps1 -Config Debug -Build -Test
`@

Or directly:

`@powershell
cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/rel
ctest --test-dir build/rel --output-on-failure
`@

Strict warnings are on by default and are errors (`/W4 /WX` on MSVC,
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` elsewhere).
The first-party warning count is zero in both configurations.

Options: `FEL_BUILD_SHARED`, `FEL_BUILD_TESTS`, `FEL_BUILD_TOOLS`,
`FEL_BUILD_EXAMPLES`, `FEL_BUILD_BENCHMARKS`, `FEL_WERROR`,
`FEL_ENABLE_ASAN`.

## Install and consume

`@powershell
cmake --install build/rel --prefix C:/fel
`@

The package exports a single target, `fel::fel`:

`@cmake
find_package(fel CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE fel::fel)
`@

`tests/downstream/` is a separate CMake project that consumes the installed
package exactly this way and is built and run as part of the release
validation. See `docs/PROOF.md` for the transcript.

## Quick start (library)

`@cpp
#include "fel/ledger.hpp"

fel::LedgerPolicy policy;
policy.freshness.fresh_ttl = fel::Duration::from_hours(24);
policy.freshness.stale_ttl = fel::Duration::from_hours(48);

auto ledger = fel::Ledger::in_memory(policy, topology, period_end);
auto period = ledger.value()->open_period(period_start, period_end, "hour-1");

fel::ObservationBatch batch;
batch.observations = observations;      // typed, fully explicit evidence
ledger.value()->ingest(batch);

auto closed = ledger.value()->close_period(
    fel::CloseRequest{period.value(), period_end, "scheduled close", "operator", false});
`@

`examples/basic_ledger.cpp`, `examples/explain_walkthrough.cpp` and
`examples/correction_lineage.cpp` are complete, buildable programs covering the
normal path, the explanation path and the correction path.

## Quick start (command line)

`@powershell
fel init      --store .\ledger --policy policy.json --topology topology.json `
              --period-start 2024-01-01T00:00:00Z --period-end 2024-01-01T01:00:00Z
fel ingest    --store .\ledger --evidence evidence.jsonl
fel close     --store .\ledger --period <id> --closed-at 2024-01-01T01:00:00Z
fel query     --store .\ledger --period <id> --json
fel reconcile --store .\ledger --period <id>
fel explain   --store .\ledger --period <id> --identity <accounting-identity>
fel export    --store .\ledger --period <id> --format csv --out rows.csv
fel verify    --store .\ledger
`@

Exit codes are `0` success, `1` runtime failure, `2` usage error.
`reconcile` and `explain` return `1` when a conservation domain is not
fully consistent or the requested cell does not exist, so they can be used
directly in a pipeline.

Evidence documents are JSON Lines: one observation object per line, or a single
envelope object with a `format_version` and an `observations` array. The
decoder is strict: unknown fields, wrong types and unsupported format versions
are refused with a specific error code rather than ignored.

---

## Accounting model

### Measures and categories

A quantity is only meaningful together with its unit. Six measure kinds are
supported: `wire-bytes`, `payload-bytes`, `port-occupancy-nanos`,
`capacity-nanos`, `control-messages`, `frame-count`. Conservation and
aggregation are computed strictly per measure kind; a computation that would mix
octets with nanoseconds is refused with `IncompatibleMeasureKind`.

Nine categories partition the accounting space. Each observation contributes to
exactly one of them, which is what makes double counting detectable rather than
merely unlikely:

| Category | Usefulness |
| --- | --- |
| `useful-delivered-work` | useful |
| `control-overhead` | necessary overhead |
| `retransmission`, `duplication`, `reroute-overhead`, `idle-reservation`, `stranded-capacity` | avoidable |
| `failed-transfer` | failed |
| `unknown-unattributed` | unknown |

An efficiency ratio is published only when it is determinate. It is
indeterminate whenever any contribution in that measure could not be measured,
whenever anything landed in the unknown bucket, or whenever a conservation
domain for that measure is not consistent. `EfficiencySummary::ratio_text()`
then reads `indeterminate (reason)` instead of a number.

### Accounting identity and double counting

A cell is identified by `(period, generation, scope, measure kind, category,
attribution basis, resource, flow, path, reservation)`. Its
`AccountingIdentity` is a SHA-256 over that tuple, so the same cell always has
the same identity on every machine.

Four independent mechanisms prevent double counting:

1. **Delivery-origin fencing.** Every observation carries an `origin`. Two
   deliveries that share an origin and agree on content are the same physical
   measurement and are counted once. Two deliveries that share an origin and
   disagree are excluded and recorded as a conflict.
2. **Evidence-identity collision detection.** Two different records claiming the
   same `EvidenceId` cannot both be real, so neither is counted and both are
   explained.
3. **Dispute resolution per cell and window.** Records that describe the same
   cell and the same reporting window are grouped. A single agreed amount is
   counted once; disagreeing amounts make the cell unknown unless the policy
   explicitly resolves them by source authority, and the resolution is recorded
   as a `ConflictRecord` either way.
4. **Disjoint scope partition.** Every resource belongs to exactly one scope, so
   hierarchical rollups sum disjoint subtrees. A resource that appears in two
   scopes is a topology validation error.

### Accounting periods and windows

An observation is attributed to the period that fully contains its reporting
window. A window that straddles a period boundary is contained in neither, so it
is refused rather than split: splitting would require inventing a proportional
allocation. The refusal is counted in the ingest report and explained in the
diagnostics.

### Conservation

Conservation is evaluated inside one `(scope, generation, measure kind)`
domain. An observation with role `total` states what the source believes the
whole domain consumed; it never contributes to a category itself. The result is
one of:

* `consistent` - attributed equals observed;
* `residual` - attributed is smaller; the difference is recorded in the unknown
  bucket as `residual-unclassified` and is **not** added to any category;
* `excess` - attributed is larger; the period is flagged as not conserved
  rather than silently reduced;
* `unverifiable` - no independent total was reported. This is not a failure and
  not a success; it is stated as such.

`ResidualPolicy::StrictReject` and `ExcessPolicy::StrictReject` turn a
residual or an excess into a hard close failure for operators who require a
complete proof before publishing a period.

### Generations, epochs, incarnations and freshness

* A resource is correlated to a fabric generation only through a
  `GenerationBinding` that covers the observation time. Evidence for a
  superseded generation is fenced as `stale-generation` and never attaches to
  current resources.
* Each source has non-overlapping incarnations with a boot epoch. Evidence that
  arrives after an incarnation is retired, or before it started, is refused.
  Evidence whose epoch is below the peak epoch seen for that incarnation is
  fenced as `fenced-epoch`.
* Freshness is always relative to an explicit `as of` instant. The runtime
  never reads the wall clock on an accounting path. Fresh, stale, expired and
  future-dated evidence are distinguished; stale evidence is excluded from
  totals by default and future-dated evidence is a clock anomaly.
* Every result carries a `ProvenanceStamp`: policy revision, topology
  revision, store revision, watermark, `as of`, fabric generations, and the
  set of provenance classes that contributed (`real`, `synthetic`,
  `replay`, `unsupported`).

### Provenance surfaces

`ProvenanceClass::Unsupported` sources are recorded but can never contribute a
number, regardless of their declared authority. Every result therefore states
which proof surfaces it rests on, and `purely_real()` is true only when
measured telemetry alone produced the result.

### Determinism

`Ledger::derive` is a pure function of `(policy, topology, period, retained
evidence, as_of)`. Evidence order, arrival order, thread scheduling and process
restart cannot change a single total. The test suite proves this by ingesting
the same evidence in five shuffled orders, by running the same ledger twice, and
by comparing a concurrent six-thread ingest against the sequential result; all
produce identical content digests and identical canonical summaries.

---

## Persistence

A store is a directory containing:

| File | Purpose |
| --- | --- |
| `fel.manifest.json` | integrity-protected index: store id, format version, segments, counters, watermark |
| `fel.lock` | OS-level exclusive lock held for the process lifetime |
| `seg-<id>.felseg` | length + CRC-32C framed records with a SHA-256 footer digest |

Recovery is conservative and never rewrites history silently. On open the
runtime verifies every segment against the manifest, reports any disagreement,
and recovers the longest valid prefix. Damage, truncation, checksum failures and
orphaned segments are surfaced in a `RecoveryReport`.
`RecoveryPolicy::Strict` turns any damage into a failed open.

Persisted dynamic evidence never silently becomes fresh: the runtime carries the
store watermark forward and re-evaluates freshness against an explicit
`as_of`. Reopening a store with an `as_of` far in the future correctly
classifies the stored evidence as expired and reports unknown rather than
republishing the old totals. This is covered by an integration test.

Closed periods are immutable. Evidence that arrives after a close is held back
and only takes effect when an operator requests a correction, which produces a
new revision whose parent digest points at the revision it supersedes. The
correction chain is linear, bounded by `max_corrections_per_period`, and
verified by `verify`.

Compaction folds the evidence of closed periods into their immutable revisions.
After compaction, the `time` and `source` aggregation axes for that period
are refused with `UnsupportedQuery` because they require retained evidence;
the `category`, `scope`, `generation`, `resource`, `flow` and
`provenance` axes are still served from the revision.

---

## Bounds

Every externally influenced size is bounded and checked, never truncated
silently: payload bytes, metadata entries and lengths, batch records, topology
and policy document sizes, worker count, queue depth, result rows, explanation
entries, conflict records, aggregation buckets, aggregation window, period span,
corrections per period, segments, segment bytes, store bytes and retained
observations. Arithmetic on externally derived sizes is checked; overflow is a
reported failure, not a wrap.

## Concurrency

One mutex guards the store state, with a documented rank and an audit that
detects reentrant acquisition and out-of-order acquisition. All public
operations are thread safe. Ingest supports a bounded queue and a worker pool
with real cancellation; shutdown joins every worker. The lock audit is checked
under load by the concurrency suite and its counters are exposed through
`fel verify` and `fel stats`.

---

## Testing

| Suite | What it covers |
| --- | --- |
| `unit` | hashing vectors, checked arithmetic, canonical JSON and CSV, time parsing, topology validation, policy parsing, freshness, claims, concurrency primitives, store framing |
| `integration` | ingest fencing, period lifecycle, conservation, aggregation, explanation, export, restart and recovery, correction lineage |
| `property` | seeded randomized evidence against the accounting invariants and determinism under shuffling and repetition |
| `adversarial` | replay, mirrored delivery, conflicting sources, epoch regression, retired incarnations, clock anomalies, undefined resources, oversized metadata, identity collisions, self-contradicting sources, arithmetic overflow |
| `concurrency` | concurrent ingest against a sequential baseline, concurrent readers, parallel ingest equivalence, lock audit under load, real cancellation |
| `e2e` | complete ingest to close to query round trips including the unsupported-source path |
| `process` | the CLI driven as separate operating system processes, proving cross-process lock exclusion and release |

No test uses a timeout of any kind. Synchronisation is by data dependency,
process exit, or a blocking pipe read.

The benchmark binaries report completed work as well as elapsed time and refuse
to exit successfully if the completed work does not match what was submitted.

## Reality of the verification

* **REAL**: everything proven on this host. The full suite, the Debug and
  Release builds, the strict-warning counts, the install and the independent
  downstream `find_package` consumer, the independent-process lock tests, and
  every benchmark number in `docs/PROOF.md`. The fabric described by tests and
  benchmarks is a declarative topology; all measurements are real computations
  over the runtime's own data structures.
* **SYNTHETIC**: the evidence used by the test suite and benchmarks is generated
  by the runtime's own fixtures and is labelled with
  `ProvenanceClass::Synthetic` where a test needs to exercise that
  distinction. No synthetic record is ever presented as telemetry from a real
  fabric.
* **UNSUPPORTED**: AddressSanitizer and ThreadSanitizer are **not available** on
  this host (the MSVC AddressSanitizer component is not installed and no LLVM
  toolchain is present), so no sanitizer run is claimed. The POSIX file-locking
  path in `src/persistence.cpp` is implemented but not exercised on this
  Windows-only host. No switch, ASIC, RDMA, InfiniBand, NVLink, multi-host or
  hardware telemetry source was available, so no claim about one is made
  anywhere in this repository.

## Limitations

* Aggregation and explanation re-run the derivation pass. The cost is linear in
  retained evidence; `benchmarks/bench_query.cpp` measures it.
* One store has one writer. Multi-writer operation is deliberately not
  supported.
* Compaction removes the raw evidence of closed periods, which disables the
  `time` and `source` aggregation axes for those periods.
* The ledger accounts for what it is told. It cannot detect a source that
  systematically under-reports and never contradicts itself; conservation
  against an independent total is the only defence, and it only works when an
  operator supplies one.
* Cost, carbon and quota semantics are out of scope.

## Documentation

* `docs/DESIGN.md` - architecture, invariants and the derivation pipeline.
* `docs/BOUNDARY.md` - the exact systems boundary and how it is enforced.
* `docs/PROOF.md` - build, test, benchmark, install and downstream transcripts.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
