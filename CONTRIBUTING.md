# Contributing

Asset Registry is an authoritative inventory store: a defect here is a wrong answer about
physical equipment. Contributions are therefore judged first on whether they preserve the
invariants below, and only then on style.

## Build and test

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Useful variants:

```
# Debug, plus the same suite with assertions and iterator debugging enabled
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --config Debug --parallel
ctest --test-dir build-debug -C Debug --output-on-failure

# AddressSanitizer over the whole suite
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DASSET_REGISTRY_SANITIZE=address
cmake --build build-asan --config RelWithDebInfo --parallel
ctest --test-dir build-asan -C RelWithDebInfo --output-on-failure

# Benchmarks (completed operations only, small scale runs in seconds)
cmake --build build --config Release --target run_benchmarks_small
```

Warnings are errors by default (`ASSET_REGISTRY_WARNINGS_AS_ERRORS=ON`). A first-party
warning is a defect: fix it rather than suppressing it, and only add a suppression with a
comment that says which defect it cannot describe.

## Invariants a change must not weaken

1. **One canonical spelling per value.** Every typed value validates on construction and has
   exactly one textual and binary form. Never add a fallback that accepts a second spelling,
   and never normalise hostile input into validity: reject it.
2. **The decoder is the trust boundary.** Durable payloads and imported documents are parsed
   by code that never reads past its buffer, never allocates before a declared count and
   length have been checked against the remaining input, and reports a structured status
   instead of throwing.
3. **Nothing unverified becomes authoritative.** A generation that fails its header
   checksum, its declared length, its payload checksum, or a record invariant is never
   published. Recovery selects the newest fully valid generation and reports what it did.
4. **A mutation is all or nothing.** It is applied to a copy, validated with the same
   predicate the store applies on load, and published in one atomic step. A failure leaves
   the published state exactly as it was.
5. **Authority and sequence are checked before state changes.** A writer session's token
   must be live for the current store epoch, and its sequence must advance; a repeated
   sequence replays only with a matching idempotency key.
6. **Ordering is part of the contract.** Assets, references, labels, provenance, lineage, and
   every enumeration have one documented order, and the durable and exported forms preserve
   it.
7. **No lock is held while user code runs, and no lock is taken twice.** The lock order is
   `commit_mutex` → (`publish_mutex` | store mutex); it has no inversions, and snapshots are
   immutable so a consumer never blocks a writer.
8. **No telemetry.** Nothing is reported anywhere except where the caller asked for it.

## Tests

The suite is the evidence for the guarantees, so a change to behaviour is a change to tests:

* Add a case that fails before the change and passes after it. A defect fix without a case
  that reproduces the defect is incomplete.
* Prefer a case that pins a property (an invariant, a round trip, a refusal) over one that
  pins an incidental value.
* Assert the refusal: the error code, and the detail a caller can act on. A test that only
  checks `!result.has_value()` does not say why the answer is no.
* Randomized cases use the seeded generator in `tests/test_support.hpp`, so a failure is
  reproducible from its seed.
* The harness has no timeout and no watchdog, deliberately: a hang is a defect to be
  diagnosed, not a case to be killed. Do not add one.

## Style

* C++20, no extensions, no third-party dependencies.
* Every file starts with the copyright line and the SPDX identifier
  (`// SPDX-License-Identifier: Apache-2.0`).
* Comments explain why a thing is the way it is, at the point where the reasoning is
  non-obvious. Comments that restate the code are noise; comments that record a decision are
  the point.
* Public headers document the contract, including what a caller must not do and which error
  a refusal produces.
* Keep the diff focused: one defect, one change, one test.

## Submitting

1. Build and run the full suite in Release and Debug, and under AddressSanitizer if the
   change touches parsing, persistence, or concurrency.
2. Describe what the change makes true that was not true before, and which case proves it.
3. Do not include generated artefacts, build trees, benchmark output, or editor state.
4. Contributions are accepted under the Apache License 2.0, as stated in `LICENSE`, with no
   additional terms.

## Reporting a defect

A useful report contains the operation, the exact input or store layout, the observed result,
and what the documented behaviour is. If the defect is in durable state, keep the store
directory: the generation files, `CURRENT`, and `meta` are the evidence, and
`asset-registry verify <store>` reproduces the integrity and conflict audit.
