# Fabric Efficiency Ledger - proof of verification

Copyright 2026 Summon Software Labs. Licensed under the Apache License 2.0.

Every transcript below was produced on the development host by running the
commands shown. Nothing is estimated, and no result is claimed that was not
observed. Where something could not be verified on this host it is listed under
**Not verified** at the end.

## Host and toolchain

`@
Operating system : Windows 11 Pro, version 10.0.26200, 64-bit
CMake            : 4.3.2
Compiler         : Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35209 for x64
                   (Visual Studio 2022 Community, toolset 14.44.35207)
Generator        : Ninja 1.13.2
Git              : 2.52.0.windows.1
`@

## Strict-warning builds

Both configurations are built from scratch with `--clean-first` and the build
log is scanned for diagnostics.

`@
=== release clean rebuild (warning scan) ===
release warnings: 0
release errors: 0
=== debug clean rebuild (warning scan) ===
debug warnings: 0
`@

MSVC flags in both configurations: `/W4 /permissive- /utf-8 /Zc:__cplusplus
/Zc:preprocessor /EHsc /WX`.

## Test suite

`@
=== release ctest ===
Test project E:/The Journey/Coding/GitHub/production/Fabric-Efficiency-Ledger/build/rel
    Start 1: unit
1/8 Test #1: unit .............................   Passed    0.11 sec
    Start 2: integration
2/8 Test #2: integration ......................   Passed    0.17 sec
    Start 3: property
3/8 Test #3: property .........................   Passed    0.21 sec
    Start 4: adversarial
4/8 Test #4: adversarial ......................   Passed    0.02 sec
    Start 5: concurrency
5/8 Test #5: concurrency ......................   Passed    0.07 sec
    Start 6: e2e
6/8 Test #6: e2e ..............................   Passed    0.01 sec
    Start 7: all
7/8 Test #7: all ..............................   Passed    0.48 sec
    Start 8: process
8/8 Test #8: process ..........................   Passed    0.10 sec

100% tests passed, 0 tests failed out of 8
=== release test case count ===
cases: 161  passed: 161  failed: 0

=== debug ctest ===
    Start 1: unit
1/8 Test #1: unit .............................   Passed    0.17 sec
    Start 2: integration
2/8 Test #2: integration ......................   Passed    0.56 sec
    Start 3: property
3/8 Test #3: property .........................   Passed    2.78 sec
    Start 4: adversarial
4/8 Test #4: adversarial ......................   Passed    0.13 sec
    Start 5: concurrency
5/8 Test #5: concurrency ......................   Passed    0.87 sec
    Start 6: e2e
6/8 Test #6: e2e ..............................   Passed    0.02 sec
    Start 7: all
7/8 Test #7: all ..............................   Passed    4.17 sec
    Start 8: process
8/8 Test #8: process ..........................   Passed    0.11 sec

100% tests passed, 0 tests failed out of 8
`@

The suites are the whole program: `fel_tests.exe` with no filter reports the
same 161 cases. No test uses a timeout; the runner has no timeout facility at
all.

## Benchmarks (Release)

Every benchmark verifies completed work before exiting successfully.

`@
--- fel_bench_ingest
ingest.sequential            completed=200000 records    elapsed=1.1891s rate=168194 per second
ingest.duplicates            completed=200000 records    elapsed=5.1715s rate=38673 per second
verified accepted=200000 retained=200000 of 200000

--- fel_bench_close
close.cells                  completed=576 cells      elapsed=0.4928s rate=1169 per second
close.records                completed=100000 records    elapsed=0.4928s rate=202918 per second
conservation domains=65 consistent=65 residual=0 excess=0 unverifiable=0
close.repeat                 completed=576 cells      elapsed=0.5153s rate=1118 per second
verified cells=576 records=100000

--- fel_bench_query
query.rows                   completed=58500 rows       elapsed=0.6680s rate=87577 per second
aggregate.buckets            completed=28850 buckets    elapsed=25.7067s rate=1122 per second
export.rows                  completed=2880 rows       elapsed=0.0830s rate=34697 per second
explain.cells                completed=576 cells      elapsed=28.2333s rate=20 per second
verified rows=58500 buckets=28850 exported=2880 explained=576

--- fel_bench_persist
persist.committed            completed=50000 records    elapsed=0.1667s rate=299902 per second
persist.recovered            completed=50000 records    elapsed=0.0180s rate=2772541 per second
store bytes=2043090 segments=50 clean=true
verified written=50000 recovered=50000
`@

Interpretation, stated plainly:

* Ingest and close are fast: 168k records per second through ingest, 203k
  records per second through the full derivation pass.
* `aggregate` and `explain` are the slow paths (1122 and 20 operations per
  second) because both re-run the derivation pass over the retained evidence.
  This is a measured cost, not a claim of efficiency.
* `persist` writes 50,000 records across 50 durable commits (one fsync per
  commit) in 0.17 s and reads all of them back in 0.018 s.

## Install and independent downstream consumer

`@
=== install ===
-- Installing: <prefix>/lib/fel.lib
-- Installing: <prefix>/bin/fel.exe
-- Installing: <prefix>/lib/cmake/fel/fel-targets.cmake
-- Installing: <prefix>/lib/cmake/fel/fel-targets-release.cmake
-- Installing: <prefix>/lib/cmake/fel/fel-config.cmake
-- Installing: <prefix>/lib/cmake/fel/fel-config-version.cmake
-- Installing: <prefix>/share/fel/LICENSE
-- Installing: <prefix>/share/fel/README.md

=== downstream find_package ===
-- Configuring done (0.0s)
-- Generating done (0.0s)
-- Build files have been written to: .../build/downstream
[1/2] Building CXX object CMakeFiles/downstream_consumer.dir/main.cpp.obj
[2/2] Linking CXX executable downstream_consumer.exe
linked against Fabric Efficiency Ledger 1.0.0
downstream consumer failures: 0
downstream exit: 0
`@

`tests/downstream/` is a separate CMake project that is not part of the
library build. It reaches the library only through
`find_package(fel CONFIG REQUIRED)` and `fel::fel`, and it exercises the
public API end to end: it builds a topology, round-trips an observation through
the public document codecs, ingests, closes, re-derives and checks that the
content digest is reproduced exactly, verifies integrity, and checks that the
proof surface is labelled `synthetic` because the evidence it created is
synthetic.

## Independent process behaviour

`@
=== independent process test ===
  ok   cli init starts
  ok   cli init exits successfully
  ok   lock holder starts
  ok   lock holder reports that it acquired the exclusive lock
  ok   contending process starts
  ok   contending process fails while the lock is held
  ok   waiting process starts
  ok   lock holder exits cleanly after its stdin closes
  ok   waiting process succeeds after the lock is released
process lock checks failed: 0
`@

The test starts the CLI as real, separate operating system processes through
`CreateProcess` with redirected standard handles. It synchronises on a line
the holder writes to stderr only after it genuinely holds the exclusive store
lock, then proves that a second process with a zero lock timeout is refused, then
releases the holder by closing its stdin and proves that a patient waiter
proceeds and succeeds. There is no sleep, no polling and no timeout: every step
is a blocking pipe read or a process exit.

## Examples

`@
--- fel_example_basic
ingest: received=3 accepted=3
period digest: 1b42347f43039b364064583c06b602684ecee094c5fa6a2a540c9354144836b0
  useful-delivered-work    known        total=900000 contributions=1
  retransmission           known        total=40000 contributions=1
  control-overhead         known        total=10000 contributions=1
  efficiency wire-bytes     coverage=partial ratio=18/19
proof surfaces: real

--- fel_example_explain
cell 7ff69199285fa422b5d3867823ba28633dfc594da760a51f85f3a02bfa3eb701
  rule             outcome                  detail
  attribution      resource-direct          scope c9e0250e022235505639655ac89889b1
  category         retransmission           avoidable
  conservation     unverifiable             attributed 12345, observed not reported
  coverage         known                    known contributions 1, unmeasured contributions 0
  double-count     fused per accounting identity
  policy           applied                  policy revision a974d510fd72bc3acaff7fe0ff865218
  topology         applied                  topology revision 95d8f6c95d3c137df048ef8822d46a56

--- fel_example_correction
revision 1 digest dccf4ee40c612636f0bb337017b6ed0ecbd35efbeff7edcc3f2eb60d25a6cf99
revision 2 digest 1b3ea07dd52cb665abbd798ec6380ee0387fa2c445f360724f473fe0576cb05c
  revision 1 total=1000
  revision 2 total=1500
integrity ok=true lineage_breaks=0
`@

The correction example shows the property that matters most: revision 1 is not
rewritten when late evidence arrives. Revision 2 is a new record whose parent
digest is revision 1, and both remain readable with their original totals.

## Hygiene scan

`@
todo markers in first-party sources and documents: 0
debug prints in the library and tooling:           0
`@

## Deliberate attempts to break the runtime

Each item below was attempted after the suite was already green. Where the
runtime was wrong, the defect and its fix are recorded.

| Attempt | Result |
| --- | --- |
| Deliver the same measurement twice through the same origin | Counted once; the second delivery is recorded as a suppressed duplicate |
| Deliver the same measurement twice through two different sources | Counted once, because both records describe the same cell and window with the same amount |
| Deliver two records with the same evidence identity and different content | Both excluded and explained; neither is counted |
| Reuse a source sequence with different content | Both excluded and recorded as a conflict |
| Let one source contradict itself inside one cell and one window | The cell becomes unknown; no winner is picked |
| Report two different amounts for one cell from two equal-authority sources | Unknown by default; a resolution policy exists but must be opted into and is always recorded |
| Report an amount higher than the independently reported total | Reported as excess; the total is not silently reduced |
| Report an amount lower than the independently reported total | The difference becomes `residual-unclassified` in the unknown bucket, not a category |
| Mix bytes and nanoseconds in one conservation equation | Impossible: domains are keyed by measure kind, and merging different kinds is refused |
| Attach usage from a superseded generation to a current resource | Fenced as `stale-generation` |
| Report evidence from an epoch below the incarnation boot epoch | Refused at ingest |
| Report evidence from an epoch below the peak epoch of its incarnation | Fenced, and a later epoch still counts |
| Send evidence from a retired incarnation | Refused |
| Date evidence after its own receive time | Clock anomaly; not counted |
| Evaluate a period far in the future | Evidence is expired; totals are unknown, not republished |
| Restart a store and query it | Freshness is re-evaluated against an explicit `as_of`; nothing is silently promoted |
| Corrupt the manifest | Rebuilt from segments under conservative recovery; strict recovery fails the open |
| Truncate a segment mid-record | The valid prefix is recovered and the discarded tail is reported |
| Flip a byte inside a record | CRC-32C catches it; the segment is reported damaged |
| Overflow an aggregate with maximal amounts | Reported as `ArithmeticOverflow` rather than wrapped |
| Exceed the retained-evidence budget | Refused with `CapacityExceeded`; nothing is silently evicted |
| Exceed the batch budget or the store byte budget | Refused with a specific capacity error |
| Straddle a period boundary with a reporting window | Refused rather than split across two periods |
| Oversize observation metadata | Refused at ingest |
| Declare a scope cycle or a resource in two scopes | Refused by topology validation |
| Open a second writer while a store is locked | Refused with `StoreLocked`, in-process and across processes |
| Reopen a store with a different policy | Refused with `PolicyRevisionMismatch` |
| Ingest from six threads concurrently | Identical content digest to the sequential run |
| Shuffle evidence into five different orders | Identical content digest and identical canonical summary |
| Cancel a worker pool with work in flight | Shutdown returns, every worker joins, in-flight count is zero |
| Run twenty million contention checks on the lock audit | Zero reentrancy detections, zero lock-order violations |

### Defects found and fixed during hardening

1. **Unmeasured evidence was invisible to the efficiency summary.** A period
   that contained a mix of measured and unusable evidence could publish a
   determinate efficiency ratio, because rejected observations never became
   claims and their contribution to the unmeasured count was not carried into
   the summary. Fixed by adding the conservation domain's unmeasured count to
   the per-measure summary, which also forces `coverage` to `partial` and
   `determinate` to false. Regression test:
   `adversarial.one_unmeasured_contribution_makes_the_ratio_indeterminate`.
2. **A test comparison could bind a reference into a destroyed temporary**,
   which made one accessor assertion read freed memory. Fixed by making the
   comparison macro copy its operands.
3. **`FieldWriter::digest()` was not idempotent**: finalising twice produced
   different digests. Fixed by caching the digest and invalidating the cache on
   any further field.
4. **Unsigned 64-bit integers lost precision through the JSON round trip**,
   because integers that did not fit a signed 64-bit value fell back to a
   floating representation. Fixed with exact unsigned storage and a strict
   two-stage parse.
5. **Field escaping used a non-hexadecimal encoding**, which made the canonical
   text ambiguous even though the digest framing was sound. Fixed with
   backslash plus two lowercase hex digits.
6. **Time aggregation aligned buckets to the epoch**, which left partial buckets
   at period edges. Fixed by aligning to the period start.
7. **Store recovery could not distinguish a torn tail from a clean segment**
   while the manifest was the only index. Fixed by sealing every segment at the
   end of each commit and verifying every segment against the manifest on open.
8. **A benchmark initially reported zero accounted cells.** The evidence
   generator had accidentally produced many contradictory records in the same
   cell and window; the runtime was correct to refuse all of them. The generator
   was fixed to be self-consistent, and the scenario became a regression test
   (`adversarial.a_self_contradicting_source_never_produces_a_partial_total`).

## Not verified

* **AddressSanitizer and ThreadSanitizer were not run.** The MSVC
  AddressSanitizer component is not installed on this host and no LLVM
  toolchain is present, so no sanitizer coverage is claimed. The
  `FEL_ENABLE_ASAN` option is implemented and passes the flags through, but it
  has not been exercised here.
* **The POSIX file-locking path is not exercised.** It is implemented in
  `src/persistence.cpp` behind `#ifndef _WIN32` and compiles nowhere on this
  Windows-only host.
* **No hardware, switch, ASIC, RDMA, InfiniBand, NVLink or multi-host transport
  was involved.** The independent-process tests prove cross-process exclusion
  on one host; they prove nothing about a network.
* **No external telemetry source was integrated.** Every observation in every
  test, benchmark and example is constructed by the runtime's own code and is
  labelled accordingly.
* **No multi-writer or clustered behaviour is claimed or tested.** It is out of
  scope by design and refused at runtime.
