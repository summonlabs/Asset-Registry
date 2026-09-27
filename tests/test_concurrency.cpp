// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Concurrency tests: concurrent readers, serialised writers, deterministic
// outcomes under interleaving, and shutdown with work in flight.
//
// The concurrency model is a single writer at a time, serialised through one
// registry mutex, with lock-free reads of an immutable published snapshot. These
// tests assert exactly that model: readers never observe a partially applied
// mutation, and concurrent writers produce outcomes that are a valid serialisation
// of their requests even though their order is not fixed.

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_harness.hpp"
#include "test_support.hpp"

using namespace asset_registry;
using namespace asset_test;

AR_TEST(concurrency, many_readers_observe_only_complete_states) {
    auto context = detached_registry();
    AssetRegistry& registry = *context.registry;
    const AssetId identifier = asset_id_for("concurrent-read");
    AR_REQUIRE_OK(created, registry.register_asset(context.session, context.session.advance(),
                                                   make_request("concurrent-read", AssetClass::Server, "Acme",
                                                                "CONC-1")));

    std::atomic_bool stop{false};
    std::atomic<std::uint64_t> observations{0};
    std::atomic<std::uint64_t> torn{0};
    constexpr int kReaders = 4;
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int index = 0; index < kReaders; ++index) {
        readers.emplace_back([&registry, &identifier, &stop, &observations, &torn] {
            while (!stop.load(std::memory_order_relaxed)) {
                const Snapshot snapshot = registry.snapshot();
                const auto view = snapshot.find(identifier);
                if (!view.has_value()) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                // A snapshot is internally consistent: the record's revision, its
                // provenance depth, and the snapshot's sequence always agree.
                if (view->provenance->size() != view->revision.value()) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
                if (view->last_sequence > snapshot.published_sequence()) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
                if (view->metadata.owner.has_value() && view->revision.value() < 2) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
                observations.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // One writer, applying many mutations while the readers spin.
    for (int index = 0; index < 200; ++index) {
        MetadataPatch patch;
        patch.owner = owner("org.example.platform");
        patch.notes = "revision " + std::to_string(index);
        auto result = registry.update_metadata(context.session, context.session.advance(), identifier,
                                               std::nullopt, patch);
        AR_CHECK(result.has_value());
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& reader : readers) {
        reader.join();
    }
    AR_CHECK(observations.load() > 0);
    AR_CHECK(torn.load() == 0);
    const auto view = registry.snapshot().find(identifier);
    AR_CHECK(view.has_value());
    AR_CHECK(view->revision == AssetRevision(201));
}

AR_TEST(concurrency, concurrent_writers_produce_a_valid_serialisation) {
    auto context = detached_registry();
    AssetRegistry& registry = *context.registry;
    constexpr int kWriters = 4;
    constexpr int kPerWriter = 40;

    // Each writer registers its own disjoint set of assets, so a correct
    // serialisation must contain every one of them exactly once.
    std::vector<std::thread> writers;
    std::vector<std::uint64_t> accepted(kWriters, 0);
    for (int index = 0; index < kWriters; ++index) {
        writers.emplace_back([&registry, index, &accepted] {
            auto session = registry.open_writer(writer("concurrent-writer-" + std::to_string(index)));
            if (!session) {
                return;
            }
            WriterSession local = session.value();
            std::uint64_t count = 0;
            for (int round = 0; round < kPerWriter; ++round) {
                const std::string label = "writer-" + std::to_string(index) + "-" + std::to_string(round);
                RegisterAssetRequest request =
                    make_request(label, AssetClass::Server, "Acme",
                                 "CW-" + std::to_string(index) + "-" + std::to_string(round));
                auto registered = registry.register_asset(local, local.advance(), request);
                if (registered.has_value()) {
                    ++count;
                }
            }
            accepted[static_cast<std::size_t>(index)] = count;
        });
    }
    for (std::thread& thread : writers) {
        thread.join();
    }

    std::uint64_t total = 0;
    for (const std::uint64_t count : accepted) {
        total += count;
    }
    AR_CHECK(total == static_cast<std::uint64_t>(kWriters * kPerWriter));
    const Snapshot snapshot = registry.snapshot();
    AR_CHECK(snapshot.size() == static_cast<std::uint64_t>(kWriters * kPerWriter));
    // Every registered asset is present, and the serial identity index is
    // consistent with the record array after concurrent structural changes.
    for (int index = 0; index < kWriters; ++index) {
        for (int round = 0; round < kPerWriter; ++round) {
            const std::string label = "writer-" + std::to_string(index) + "-" + std::to_string(round);
            const auto view = snapshot.find(asset_id_for(label));
            AR_CHECK_MSG(view.has_value(), label);
            if (!view.has_value()) {
                continue;
            }
            const auto by_serial =
                snapshot.find_by_serial(serial("Acme", "CW-" + std::to_string(index) + "-" + std::to_string(round)));
            AR_CHECK(by_serial.has_value());
            if (by_serial.has_value()) {
                AR_CHECK(*by_serial == view->id);
            }
        }
    }
    AR_CHECK(snapshot.audit_conflicts().clean());
    // The transaction sequence advanced once per accepted mutation.
    AR_CHECK(snapshot.published_sequence().value() == total);
}

AR_TEST(concurrency, stale_writers_lose_exactly_once_under_contention) {
    auto context = detached_registry();
    AssetRegistry& registry = *context.registry;
    const AssetId identifier = asset_id_for("contended-asset");
    AR_REQUIRE_OK(created, registry.register_asset(context.session, context.session.advance(),
                                                   make_request("contended-asset", AssetClass::Server, "Acme",
                                                                "CONT-1")));

    // Every thread reads revision 1 and then tries to write against it. Exactly one
    // may succeed, and every other attempt must be refused as stale rather than
    // silently overwriting the winner.
    constexpr int kThreads = 8;
    std::atomic<std::uint64_t> successes{0};
    std::atomic<std::uint64_t> stale_rejections{0};
    std::atomic<std::uint64_t> other{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&registry, identifier, index, &successes, &stale_rejections, &other] {
            auto session = registry.open_writer(writer("contender-" + std::to_string(index)));
            if (!session) {
                other.fetch_add(1);
                return;
            }
            WriterSession local = session.value();
            MetadataPatch patch;
            patch.notes = "contender " + std::to_string(index);
            auto result = registry.update_metadata(local, local.advance(), identifier, AssetRevision(1), patch);
            if (result.has_value()) {
                successes.fetch_add(1);
            } else if (result.code() == ErrorCode::StaleRevision) {
                stale_rejections.fetch_add(1);
            } else {
                other.fetch_add(1);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    AR_CHECK(successes.load() == 1);
    AR_CHECK(stale_rejections.load() + other.load() == static_cast<std::uint64_t>(kThreads - 1));
    AR_CHECK(other.load() == 0);
    const auto view = registry.snapshot().find(identifier);
    AR_CHECK(view.has_value());
    AR_CHECK(view->revision == AssetRevision(2));
}

AR_TEST(concurrency, repeated_open_and_close_is_stable) {
    TempDirectory directory("concurrency-reopen");
    for (int round = 0; round < 6; ++round) {
        auto context = open_registry(directory.path());
        auto registered = context.registry->register_asset(
            context.session, context.session.advance(),
            make_request("reopen-" + std::to_string(round), AssetClass::Server, "Acme",
                         "RO-" + std::to_string(round)));
        AR_CHECK(registered.has_value());
        AR_CHECK(context.registry->snapshot().size() == static_cast<std::uint64_t>(round + 1));
        AR_CHECK(context.registry->flush().has_value());
        AR_CHECK(context.registry->close().has_value());
        // Closing again is harmless.
        AR_CHECK(context.registry->close().has_value());
        // Mutating after close is refused with the lifecycle code, not a crash.
        auto refused = context.registry->register_asset(
            context.session, context.session.advance(),
            make_request("after-close-" + std::to_string(round), AssetClass::Server, "Acme",
                         "AC-" + std::to_string(round)));
        AR_CHECK(!refused.has_value());
        if (!refused.has_value()) {
            AR_CHECK(refused.error().code() == ErrorCode::RegistryClosed);
        }
    }
    // The store holds exactly the assets that were committed.
    auto final_context = open_registry(directory.path());
    AR_CHECK(final_context.registry->snapshot().size() == 6);
    AR_CHECK(final_context.registry->close().has_value());
}

AR_TEST(concurrency, closing_with_work_in_flight_does_not_deadlock_or_publish) {
    auto context = detached_registry();
    AssetRegistry& registry = *context.registry;

    // The writer keeps mutating until it has observed the close, so the number of
    // refusals is a property of the close rather than of how quickly two hundred
    // in-memory mutations happen to finish. The round bound only keeps the case
    // finite if the registry never refuses at all, which is the fault under test.
    constexpr int kRoundBound = 20000;
    constexpr std::uint64_t kRefusalsBeforeStopping = 8;

    std::atomic_bool proceed{false};
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> refused{0};
    std::thread writer([&registry, &proceed, &accepted, &refused] {
        auto session = registry.open_writer(writer("shutdown-writer"));
        if (!session) {
            return;
        }
        WriterSession local = session.value();
        while (!proceed.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
        }
        for (int round = 0; round < kRoundBound; ++round) {
            RegisterAssetRequest request =
                make_request("shutdown-" + std::to_string(round), AssetClass::Server, "Acme",
                             "SD-" + std::to_string(round));
            auto registered = registry.register_asset(local, local.advance(), request);
            if (registered.has_value()) {
                accepted.fetch_add(1);
                continue;
            }
            if (refused.fetch_add(1) + 1 >= kRefusalsBeforeStopping) {
                break;
            }
        }
    });

    proceed.store(true, std::memory_order_relaxed);
    // Close while the writer is running. Every mutation either completed before the
    // close or is refused; none is left half-applied.
    while (accepted.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }
    auto closed = registry.close();
    AR_CHECK(closed.has_value());
    writer.join();

    AR_CHECK(accepted.load() + refused.load() <= static_cast<std::uint64_t>(kRoundBound));
    AR_CHECK(refused.load() > 0);
    const Snapshot snapshot = registry.snapshot();
    // Whatever was accepted is present and consistent.
    AR_CHECK(snapshot.size() == accepted.load());
    for (const AssetId& identifier : snapshot.ids()) {
        const auto view = snapshot.find(identifier);
        AR_CHECK(view.has_value());
        AR_CHECK(view->revision == AssetRevision(1));
    }
    // The registry refuses further work.
    auto after = registry.register_asset(context.session, context.session.advance(),
                                         make_request("after-shutdown", AssetClass::Server, "Acme", "AS-1"));
    AR_REQUIRE_ERROR(error, after, ErrorCode::RegistryClosed);
}

AR_TEST(concurrency, authority_revocation_is_immediate_for_every_copy) {
    auto context = detached_registry();
    AssetRegistry& registry = *context.registry;
    WriterSession session = context.session;
    // Copy the session: the copy holds the same token, and revoking through one
    // path must invalidate both.
    WriterSession copy = session;
    AR_CHECK(registry.close_writer(session).has_value());
    auto refused = registry.register_asset(copy, copy.advance(),
                                           make_request("revoked-copy", AssetClass::Server, "Acme", "REV-1"));
    AR_REQUIRE_ERROR(error, refused, ErrorCode::AuthorityRevoked);
    AR_CHECK(registry.snapshot().empty());
}

AR_TEST(concurrency, concurrent_registration_of_one_serial_identity_yields_exactly_one_asset) {
    auto context = detached_registry();
    AssetRegistry& registry = *context.registry;
    constexpr int kThreads = 6;
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> duplicates{0};
    std::atomic<std::uint64_t> other{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&registry, index, &accepted, &duplicates, &other] {
            auto session = registry.open_writer(writer("serial-racer-" + std::to_string(index)));
            if (!session) {
                other.fetch_add(1);
                return;
            }
            WriterSession local = session.value();
            RegisterAssetRequest request = make_request("serial-racer-" + std::to_string(index), AssetClass::Server,
                                                       "Acme", "RACE-1");
            auto registered = registry.register_asset(local, local.advance(), request);
            if (registered.has_value()) {
                accepted.fetch_add(1);
            } else if (registered.code() == ErrorCode::DuplicateSerialIdentity) {
                duplicates.fetch_add(1);
            } else {
                other.fetch_add(1);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    AR_CHECK(accepted.load() == 1);
    AR_CHECK(duplicates.load() == static_cast<std::uint64_t>(kThreads - 1));
    AR_CHECK(other.load() == 0);
    const Snapshot snapshot = registry.snapshot();
    AR_CHECK(snapshot.size() == 1);
    AR_CHECK(snapshot.audit_conflicts().clean());
}

AR_TEST(concurrency, concurrent_durable_writers_through_one_registry_are_consistent) {
    TempDirectory directory("concurrency-durable");
    auto context = open_registry(directory.path());
    AssetRegistry& registry = *context.registry;
    constexpr int kThreads = 3;
    constexpr int kPerThread = 15;
    std::atomic<std::uint64_t> accepted{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&registry, index, &accepted] {
            auto session = registry.open_writer(writer("durable-racer-" + std::to_string(index)));
            if (!session) {
                return;
            }
            WriterSession local = session.value();
            for (int round = 0; round < kPerThread; ++round) {
                RegisterAssetRequest request =
                    make_request("durable-racer-" + std::to_string(index) + "-" + std::to_string(round),
                                 AssetClass::Server, "Acme",
                                 "DR-" + std::to_string(index) + "-" + std::to_string(round));
                auto registered = registry.register_asset(local, local.advance(), request);
                if (registered.has_value()) {
                    accepted.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    AR_CHECK(accepted.load() == static_cast<std::uint64_t>(kThreads * kPerThread));
    const TransactionSequence committed = registry.published_sequence();
    AR_CHECK(committed.value() == accepted.load());
    AR_CHECK(registry.close().has_value());

    auto reopened = open_registry(directory.path());
    AR_CHECK(reopened.registry->snapshot().size() == accepted.load());
    AR_CHECK(reopened.registry->snapshot().published_sequence() == committed);
    for (int index = 0; index < kThreads; ++index) {
        for (int round = 0; round < kPerThread; ++round) {
            AR_CHECK(reopened.registry
                         ->snapshot()
                         .find(asset_id_for("durable-racer-" + std::to_string(index) + "-" + std::to_string(round)))
                         .has_value());
        }
    }
    AR_CHECK(reopened.registry->close().has_value());
}
