# Asset Registry

Authoritative physical asset inventory and lifecycle registry for DCCP Tranche 1.

The registry answers one question for a facility: *what physical objects exist, where are
they, what state are they in, and what evidence supports that?* It stores that answer as a
durable, checksummed inventory whose every value has exactly one canonical spelling, and it
refuses anything it cannot prove: an unknown asset class, a duplicate serial identity, a
stale revision, a replayed mutation sequence, a payload that fails its checksum, a
generation whose ancestry does not match what was committed.

It is a library (`AssetRegistry::AssetRegistry`) plus an inspection and mutation CLI
(`asset-registry`). There are no third-party dependencies: C++20 standard library only, with
the operating system's cryptographic random generator for identity entropy.

## What it is not

The registry holds inventory records. It does not talk to accelerators, PDUs, UPS units,
cooling equipment, or any other facility hardware, and nothing here claims otherwise. It
does not compute rack occupancy or resolve topology conflicts: those belong to the facility
topology repository, and the registry records references to them with an evidence level.

## Building

Requirements: CMake 3.20 or newer, and a C++20 compiler (MSVC 19.3x, GCC 11+, or Clang 14+).

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Options, all `ON` by default except where noted:

| Option | Effect |
| --- | --- |
| `ASSET_REGISTRY_BUILD_SHARED` | Build the library as a shared library (`OFF` builds it static). |
| `ASSET_REGISTRY_BUILD_TESTS` | Build the test suite. Defaults to `ON` for a top-level build. |
| `ASSET_REGISTRY_BUILD_CLI` | Build the `asset-registry` CLI. |
| `ASSET_REGISTRY_BUILD_EXAMPLES` | Build the examples, which also run as tests. |
| `ASSET_REGISTRY_BUILD_BENCHMARKS` | Build the benchmark driver. |
| `ASSET_REGISTRY_WARNINGS_AS_ERRORS` | Treat first-party warnings as errors. |
| `ASSET_REGISTRY_SANITIZE` | `address` or `thread` instrumentation for first-party targets. |

`ASSET_REGISTRY_SANITIZE=address` builds the suite under AddressSanitizer; the same suite
passes with no findings.

## Using the library

```cmake
find_package(AssetRegistry 1.0 REQUIRED)
target_link_libraries(your_tool PRIVATE AssetRegistry::AssetRegistry)
```

```cpp
#include "asset_registry/asset_registry.hpp"

using namespace asset_registry;

RecoveryReport report;
auto opened = AssetRegistry::open("/var/lib/asset-registry", StoreOpenMode::CreateIfMissing,
                                  StoreOpenOptions{}, report);
if (!opened) {
    std::fprintf(stderr, "%s\n", opened.error().to_string().c_str());
    return 1;
}
std::shared_ptr<AssetRegistry> registry = opened.value();

// Every mutation is presented by a writer session: a writer identity, an authority token
// minted by the open store, and a mutation sequence that advances for every attempt.
auto session = registry->open_writer(WriterId::create("change-window-1").value());
if (!session) {
    std::fprintf(stderr, "%s\n", session.error().to_string().c_str());
    return 1;
}
WriterSession writer_session = session.value();

RegisterAssetRequest request;
request.id = AssetId::generate().value();
request.asset_class = AssetClass::Server;
request.serial_identity = SerialIdentity::parse("Acme//SN-1234").value();
request.metadata.display_name = "compute-1";
request.metadata.owner = OwnerId::create("org.example.platform").value();
request.lifecycle = LifecycleState::Planned;

MutationEnvelope envelope = writer_session.advance("register-1", "first registration");
auto registered = registry->register_asset(writer_session, envelope, request);
if (!registered) {
    std::fprintf(stderr, "refused: %s\n", registered.error().to_string().c_str());
    return 1;
}

// A snapshot is immutable and lock-free: it keeps answering from the state it was taken
// at, whatever the next mutation does.
const Snapshot snapshot = registry->snapshot();
const InventorySummary summary = snapshot.summary();
std::printf("%llu assets\n", static_cast<unsigned long long>(summary.total_assets));
registry->close();
```

The CLI wraps the same calls:

```
asset-registry info    /var/lib/asset-registry
asset-registry list    /var/lib/asset-registry --limit 20
asset-registry verify  /var/lib/asset-registry
asset-registry export  /var/lib/asset-registry --out inventory.json
asset-registry register /var/lib/asset-registry record.json --writer change-window-1
```

`asset-registry --help` lists every command. Read commands open the store read-only and take
no lock, so an inspection never blocks a writer. Mutations require `--writer`.

## Durable store layout

```
<store>/
  meta                 policy fingerprint, epoch, published sequence, retention floor
  CURRENT              the generation currently authoritative
  lock                 exclusive, non-blocking cross-process lock
  generations/         gen-<20-digit sequence>.dat, one file per committed transaction
  tmp/                 staging files; a file here was never published
```

A generation file is a 48-byte header, the payload, and a 4-byte CRC-32 trailer. The header
carries a magic number, the format version, the payload schema version, the transaction
sequence, the registry epoch, the declared payload length, and a CRC-32 over the first 40
header bytes. Nothing is trusted before it is checked: header checksum first, then the
declared length against the actual file size, then the payload checksum, then a full decode.

A commit stages the complete image, re-reads it and verifies its checksums, atomically
replaces the destination, and only then publishes the pointer and flushes the metadata
record. A failure at any point leaves the previous generation authoritative.

The payload is a canonical tag/length/value encoding: every integer has exactly one width,
every list carries its element count, every enumeration is a number checked against its
domain, and records, references, and labels are emitted in ascending canonical order. The
decoder never reads past its buffer, never allocates before a declared count and length have
been validated against the remaining input, and reports a structured status instead of
throwing. It is the trust boundary for every durable artefact and every imported document.

## Recovery, fencing, and retention

* Recovery selects the highest-numbered fully valid generation and reports what it did:
  `None`, `RolledBackToLastValid`, `DamagedGenerationQuarantined`,
  `RemovedOrphanTemporaries`, or `Initialised`. A damaged generation is never published, and
  a store whose only generation is damaged is refused rather than silently re-created.
* Opening a store mints a new epoch. A writer session armed under an earlier epoch is fenced
  out, in this process and in any other.
* A writer's sequence advances for every attempt. A repeated sequence with a matching
  idempotency key replays the recorded outcome; without a key it is refused. Rejections are
  recorded too, so an attempt that arrives after a restart observes the same answer.
* Retained generations are bounded (`max_retained_generations`, default 64, minimum 2); the
  oldest are removed only after the replacement is published, and the retention floor is
  recorded so a hand-edited pointer cannot be papered over.
* Per-asset provenance is bounded (`max_provenance_per_asset`, default 4096). At the bound,
  the oldest middle steps are replaced by one compaction step that states how many it
  replaced, while the origin and the newest steps are kept.
* The cross-process lock is exclusive and non-blocking, so a second writer is refused
  immediately instead of waiting, and the operating system releases it if a process dies
  mid-operation.

## Guarantees the tests prove

The suite is the specification's evidence, and each case follows an assertion rather than a
convenience. `tests/` contains:

| Target | What it pins |
| --- | --- |
| `ar_test_values` | Typed values, canonical spellings, text rules, unit spans, timestamps. |
| `ar_test_codec` | The framed codec and the durable payload codec, byte for byte, including malformed framing. |
| `ar_test_wire` | The durable payload's exact bytes and fixed point behaviour. |
| `ar_test_state_machine` | Lifecycle and installation transition tables, closure, and refusal messages. |
| `ar_test_registration` | Registration, derived identities, revisions, expectations, capacity. |
| `ar_test_mutations` | Metadata, placement, serial, references, replacement links, provenance compaction. |
| `ar_test_durability` | Close/reopen round trips, corruption and truncation, conservative recovery, retention, retry replay, stale revisions. |
| `ar_test_derive` | Identity derivation, lineage chains and fan-out, conflict audit, cycle refusal. |
| `ar_test_documents` | Canonical export, import batches, conflict policies, accounting closure. |
| `ar_test_property` | Seeded randomized sequences: operations, invariants, lineage walks, replay. |
| `ar_test_adversarial` | Hostile text, malformed documents, integer edges, oversized batches, authority abuse. |
| `ar_test_concurrency` | Readers during writes, concurrent writers, close during a mutation, revocation, race proof. |
| `ar_test_multiprocess` | Real processes: cross-process authority, lock exclusion, epoch fencing, kill and restart. |

The harness has no timeout and no watchdog: a case that hangs hangs the run, because a hang
is a defect to be diagnosed rather than a case to be killed.

## Concurrency and locking

The library's locking is deliberately small, and the audit that keeps it small is by
inspection:

* `RegistryImpl::commit_mutex` serialises mutations from one process. It is taken by every
  mutation, by `close()`, and by the authority paths that mint grants, and it is held for the
  whole mutation.
* `RegistryImpl::publish_mutex` guards only the published-body pointer. It is taken for the
  duration of one shared-pointer copy or swap and released before anything else runs.
* `Store::Impl::mutex` serialises one store's commit, close, and grant table. It is always
  taken *inside* a commit mutex and never the other way round, so the order is
  `commit_mutex` → `publish_mutex` and `commit_mutex` → store mutex, with no inversions.
* The index cache takes its own mutex, releases it, builds outside the lock, and only then
  swaps the pointer in. A body has exactly one sequence, so a cached entry is never replaced
  by a different one and a reference handed out stays valid for the body's lifetime.
* No user code runs under any lock. A snapshot owns an immutable body, so enumeration,
  filtering, lineage walks, and audit reports read it with no lock at all, and a visitor may
  call back into the registry.
* No lock is recursive, and no locked path calls another locked entry point: `close()` does
  not take the commit mutex again, and the store never calls back into the registry.
* Cross-process exclusion uses one exclusive non-blocking file lock, so contention is a
  refusal rather than a wait, and a dead process cannot leave the store locked.

## Benchmarks

Benchmarks measure completed operations only: the clock stops after the operation returns
success, and where durability is part of the operation the full commit cost — staging,
flushing, publishing the pointer, and flushing the metadata record — is inside the
measurement. Nothing measures submission or enqueue latency, and the numbers describe the
machine that produced them, with its configuration printed alongside.

```
cmake --build build --config Release --target run_benchmarks_small   # seconds
cmake --build build --config Release --target run_benchmarks         # standard scale
```

## Installing and consuming

```
cmake --install build --config Release --prefix /opt/asset-registry
```

This installs the library, the public headers, the CLI, the examples, the package
configuration, and this documentation. A downstream project then uses
`find_package(AssetRegistry 1.0 REQUIRED)` and links `AssetRegistry::AssetRegistry`; no
absolute build-tree path is baked into the package, so it works from any install prefix.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
