// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Property and state-machine tests over long randomised sequences.
//
// Every stream is produced by the seeded generator in test_support, and every
// case prints its seed, so a failing run is reproducible exactly. The properties
// are asserted against an independently maintained model where the model is
// simple enough to be obviously right.

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "test_harness.hpp"
#include "test_support.hpp"

using namespace asset_registry;
using namespace asset_test;

namespace {

/// Independent model of one asset's observable state.
struct ModelAsset {
    AssetRevision revision{1};
    AssetState state;
    AssetMetadata metadata;
    std::vector<Reference> references;
    std::uint32_t generation = 1;
};

/// The properties every reachable registry state must satisfy, checked from the
/// published snapshot alone. A violation means the registry published something it
/// should not have, or lost an invariant on the way.
void check_snapshot_invariants(const Snapshot& snapshot) {
    std::vector<AssetId> identifiers = snapshot.ids();
    for (std::size_t index = 1; index < identifiers.size(); ++index) {
        AR_CHECK(identifiers[index - 1] < identifiers[index]);
    }
    std::set<std::string> serial_keys;
    for (const AssetId& identifier : identifiers) {
        const auto view = snapshot.find(identifier);
        if (!view.has_value()) {
            AR_CHECK_MSG(false, "snapshot ids() named an asset that find() could not resolve: " +
                                    identifier.to_string());
            continue;
        }
        AR_CHECK(!identifier.is_nil());
        AR_CHECK(is_known(view->asset_class));
        AR_CHECK(view->generation.is_zero() == false);
        AR_CHECK(view->revision.is_zero() == false);
        AR_CHECK(!view->serial_identity.empty());
        AR_CHECK(!view->metadata.display_name.empty());
        // References are canonically ordered and unique.
        for (std::size_t ref = 1; ref < view->references.size(); ++ref) {
            AR_CHECK(view->references[ref - 1].canonical() < view->references[ref].canonical());
        }
        // Labels are strictly ascending by key.
        for (std::size_t label = 1; label < view->metadata.labels.size(); ++label) {
            AR_CHECK(view->metadata.labels[label - 1].first < view->metadata.labels[label].first);
        }
        // Serial identity uniqueness across the whole inventory.
        AR_CHECK(serial_keys.insert(view->serial_identity.canonical_key()).second);
        // The state pair satisfies the documented consistency rules.
        AR_CHECK(!check_state_consistency(view->state.lifecycle, view->state.installation,
                                         default_consistency_rules())
                      .has_value());
        // A terminal state never permits workload.
        if (is_terminal_lifecycle(view->state.lifecycle)) {
            AR_CHECK(!permits_workload(view->state.lifecycle));
        }
        // Provenance is ordered, bounded, and agrees with the record.
        if (view->provenance != nullptr && !view->provenance->empty()) {
            const std::vector<ProvenanceStep>& steps = *view->provenance;
            AR_CHECK(steps.size() <= snapshot.bounds().max_provenance_per_asset);
            AR_CHECK(steps.back().revision == view->revision);
            AR_CHECK(steps.back().sequence == view->last_sequence);
            for (std::size_t step = 1; step < steps.size(); ++step) {
                AR_CHECK(steps[step - 1].sequence < steps[step].sequence);
            }
        }
        // Lineage never points at the record itself and never cycles.
        if (view->supersedes.has_value()) {
            AR_CHECK(!(view->supersedes->predecessor == identifier));
        }
        const auto chain = snapshot.lineage(identifier, limits::kMaxLineageDepth);
        AR_CHECK(chain.has_value());
        if (chain.has_value()) {
            std::set<std::string> seen;
            for (const AssetId& ancestor : chain.value().predecessors) {
                AR_CHECK(seen.insert(ancestor.to_compact_string()).second);
                AR_CHECK(!(ancestor == identifier));
            }
        }
    }
    // The audit finds no defect in state the registry itself produced.
    const ConflictReport report = snapshot.audit_conflicts();
    AR_CHECK(report.clean());
}

/// Whether a transition proposal is admissible under the model, so the model and
/// the registry agree about which changes should be accepted.
[[nodiscard]] bool model_allows_transition(const ModelAsset& model, LifecycleState target) {
    if (model.state.lifecycle == target) {
        return false;
    }
    if (!is_legal_lifecycle_transition(model.state.lifecycle, target)) {
        return false;
    }
    AssetState candidate = model.state;
    candidate.lifecycle = target;
    return !check_state_consistency(candidate.lifecycle, candidate.installation, default_consistency_rules())
                .has_value();
}

[[nodiscard]] bool model_allows_installation(const ModelAsset& model, InstallationState target) {
    if (model.state.installation == target) {
        return false;
    }
    if (!is_legal_installation_transition(model.state.installation, target)) {
        return false;
    }
    AssetState candidate = model.state;
    candidate.installation = target;
    return !check_state_consistency(candidate.lifecycle, candidate.installation, default_consistency_rules())
                .has_value();
}

}  // namespace

AR_TEST(property, long_random_edit_sequences_agree_with_an_independent_model) {
    // Several seeds, each producing a few thousand operations, so the suite covers
    // far more state than an example-based test could.
    const std::uint64_t seeds[] = {1, 2, 3, 12345, 0xC0FFEEULL, 0xDEADBEEFULL};
    for (const std::uint64_t seed : seeds) {
        SeededRandom random(seed);
        auto context = detached_registry();
        AssetRegistry& registry = *context.registry;
        WriterSession& session = context.session;

        std::map<std::string, ModelAsset> model;
        std::vector<std::string> labels;

        for (int step = 0; step < 400; ++step) {
            const std::uint64_t choice = random.below(100);
            if (choice < 35 || labels.empty()) {
                const std::string label = "prop-" + std::to_string(seed) + "-" + std::to_string(step);
                RegisterAssetRequest request =
                    make_request(label, random.coin() ? AssetClass::Server : AssetClass::Switch, "Acme",
                                 "PROP-" + std::to_string(seed) + "-" + std::to_string(step));
                request.metadata.owner = owner("org.example.platform");
                auto registered = registry.register_asset(session, session.advance(), request);
                AR_CHECK_MSG(registered.has_value(), registered.has_value() ? "" : registered.error().to_string());
                if (!registered.has_value()) {
                    continue;
                }
                ModelAsset entry;
                entry.state = AssetState{request.lifecycle, request.installation};
                entry.metadata = request.metadata;
                model.emplace(label, entry);
                labels.push_back(label);
                continue;
            }

            const std::string& label = labels[random.below(labels.size())];
            ModelAsset& entry = model[label];
            const AssetId identifier = asset_id_for(label);
            switch (choice) {
                case 35:
                case 36:
                case 37: {
                    // Metadata patch.
                    MetadataPatch patch;
                    if (random.coin()) {
                        patch.notes = "note-" + std::to_string(random.next());
                    }
                    if (random.coin()) {
                        patch.owner = owner(random.coin() ? "org.example.platform" : "org.example.other");
                    }
                    if (random.coin()) {
                        patch.set_labels = {{"probe", std::to_string(random.next())}};
                    }
                    if (random.coin()) {
                        patch.display_name = label + "-renamed-" + std::to_string(random.next());
                    }
                    if (!patch.display_name.has_value() && !patch.owner.has_value() && !patch.notes.has_value() &&
                        patch.set_labels.empty() && !patch.clear_owner && patch.remove_labels.empty()) {
                        continue;
                    }
                    auto result = registry.update_metadata(session, session.advance(), identifier,
                                                           entry.revision, patch);
                    if (result.has_value()) {
                        if (patch.display_name.has_value()) {
                            entry.metadata.display_name = *patch.display_name;
                        }
                        if (patch.owner.has_value()) {
                            entry.metadata.owner = *patch.owner;
                        }
                        if (patch.notes.has_value()) {
                            entry.metadata.notes = *patch.notes;
                        }
                        entry.revision = result.value().revision;
                    }
                    break;
                }
                case 38:
                case 39:
                case 40:
                case 41: {
                    // Lifecycle transition.
                    const LifecycleState targets[] = {LifecycleState::Planned,
                                                      LifecycleState::Provisioned,
                                                      LifecycleState::Active,
                                                      LifecycleState::Maintenance,
                                                      LifecycleState::Decommissioned,
                                                      LifecycleState::Disposed};
                    const LifecycleState target = targets[random.below(6)];
                    const bool allowed = model_allows_transition(entry, target);
                    auto result = registry.transition_lifecycle(session, session.advance(), identifier,
                                                                entry.revision, target);
                    if (allowed) {
                        AR_CHECK(result.has_value());
                        if (result.has_value()) {
                            entry.state.lifecycle = target;
                            entry.revision = result.value().revision;
                        }
                    } else {
                        AR_CHECK(!result.has_value());
                    }
                    break;
                }
                case 42:
                case 43:
                case 44:
                case 45: {
                    // Installation transition.
                    const InstallationState targets[] = {InstallationState::NotInstalled,
                                                         InstallationState::Staged,
                                                         InstallationState::Installed,
                                                         InstallationState::Removed,
                                                         InstallationState::Superseded};
                    const InstallationState target = targets[random.below(5)];
                    const bool allowed = model_allows_installation(entry, target);
                    auto result = registry.transition_installation(session, session.advance(), identifier,
                                                                    entry.revision, target);
                    if (allowed) {
                        AR_CHECK(result.has_value());
                        if (result.has_value()) {
                            entry.state.installation = target;
                            entry.revision = result.value().revision;
                        }
                    } else {
                        AR_CHECK(!result.has_value());
                    }
                    break;
                }
                case 46:
                case 47:
                case 48: {
                    // Reference attachment.
                    const Reference reference =
                        capability_ref("asi:pool-" + std::to_string(random.below(4)), ReferenceEvidence::Unverified);
                    const bool already = std::find(entry.references.begin(), entry.references.end(), reference) !=
                                         entry.references.end();
                    auto result = registry.attach_reference(session, session.advance(), identifier, entry.revision,
                                                            reference);
                    if (!already && result.has_value()) {
                        entry.references.push_back(reference);
                        std::sort(entry.references.begin(), entry.references.end());
                        entry.revision = result.value().revision;
                    } else if (already) {
                        AR_CHECK(!result.has_value());
                        if (!result.has_value()) {
                            AR_CHECK(result.code() == ErrorCode::ReferenceAlreadyAttached);
                        }
                    }
                    break;
                }
                case 49:
                case 50: {
                    // Reference detachment.
                    if (entry.references.empty()) {
                        continue;
                    }
                    const Reference reference = entry.references[random.below(entry.references.size())];
                    auto result = registry.detach_reference(session, session.advance(), identifier, entry.revision,
                                                            reference);
                    AR_CHECK(result.has_value());
                    if (result.has_value()) {
                        entry.references.erase(
                            std::remove(entry.references.begin(), entry.references.end(), reference),
                            entry.references.end());
                        entry.revision = result.value().revision;
                    }
                    break;
                }
                case 51:
                case 52: {
                    // Reclassification.
                    const AssetClass targets[] = {AssetClass::Server, AssetClass::Switch,
                                                  AssetClass::StorageAppliance};
                    const AssetClass target = targets[random.below(3)];
                    auto result = registry.reclassify_asset(session, session.advance(), identifier, entry.revision,
                                                            target);
                    if (result.has_value()) {
                        entry.revision = result.value().revision;
                    }
                    break;
                }
                default: {
                    // Stale-read attempt: a revision the record has already passed
                    // must never be accepted.
                    if (entry.revision.value() <= 1) {
                        continue;
                    }
                    const AssetRevision stale(entry.revision.value() - 1);
                    MetadataPatch patch;
                    patch.notes = "stale attempt " + std::to_string(random.next());
                    auto result = registry.update_metadata(session, session.advance(), identifier, stale, patch);
                    AR_CHECK(!result.has_value());
                    if (!result.has_value()) {
                        AR_CHECK(result.code() == ErrorCode::StaleRevision);
                    }
                    break;
                }
            }
        }

        // The model and the registry agree about every record.
        const Snapshot snapshot = registry.snapshot();
        AR_CHECK(snapshot.size() == model.size());
        for (const auto& entry : model) {
            const auto view = snapshot.find(asset_id_for(entry.first));
            AR_CHECK_MSG(view.has_value(), entry.first);
            if (!view.has_value()) {
                continue;
            }
            AR_CHECK(view->revision == entry.second.revision);
            AR_CHECK(view->state == entry.second.state);
            AR_CHECK(view->metadata.owner == entry.second.metadata.owner);
            AR_CHECK(view->references == entry.second.references);
        }
        check_snapshot_invariants(snapshot);
    }
}

AR_TEST(property, random_registration_and_retirement_keeps_serial_uniqueness) {
    const std::uint64_t seeds[] = {7, 99, 4242};
    for (const std::uint64_t seed : seeds) {
        SeededRandom random(seed);
        auto context = detached_registry();
        AssetRegistry& registry = *context.registry;
        std::map<std::string, AssetId> by_serial;

        for (int step = 0; step < 300; ++step) {
            // A small serial space so collisions are frequent rather than rare.
            const std::string serial_number = "SER-" + std::to_string(random.below(40));
            const std::string manufacturer = random.coin() ? "Acme" : "acme";
            const std::string label = "uniq-" + std::to_string(seed) + "-" + std::to_string(step);
            RegisterAssetRequest request =
                make_request(label, AssetClass::Server, manufacturer, serial_number);
            auto registered = registry.register_asset(context.session, context.session.advance(), request);
            const std::string key = serial(manufacturer, serial_number).canonical_key();
            const bool expected_new = by_serial.find(key) == by_serial.end();
            if (registered.has_value()) {
                AR_CHECK_MSG(expected_new, "registration succeeded for a serial already claimed");
                by_serial.emplace(key, registered.value().id);
            } else {
                AR_CHECK_MSG(!expected_new, registered.error().to_string());
                if (!expected_new) {
                    AR_CHECK(registered.code() == ErrorCode::DuplicateSerialIdentity);
                }
            }
        }
        const Snapshot snapshot = registry.snapshot();
        AR_CHECK(snapshot.size() == by_serial.size());
        for (const auto& entry : by_serial) {
            const auto claimant = snapshot.find_by_serial(serial("Acme", entry.first.substr(entry.first.find('/') + 1)));
            AR_CHECK(claimant.has_value());
            if (claimant.has_value()) {
                AR_CHECK(*claimant == entry.second);
            }
        }
        check_snapshot_invariants(snapshot);
    }
}

AR_TEST(property, random_replacement_chains_stay_acyclic_and_complete) {
    RegistryPolicy policy;
    policy.serial_collision = SerialCollisionPolicy::AllowReplacementOfTerminal;
    const std::uint64_t seeds[] = {11, 222, 3333};
    for (const std::uint64_t seed : seeds) {
        SeededRandom random(seed);
        auto context = detached_registry(policy);
        AssetRegistry& registry = *context.registry;
        std::vector<AssetId> live;
        std::set<std::string> all_identities;

        for (int step = 0; step < 200; ++step) {
            if (live.empty() || random.below(100) < 45) {
                RegisterAssetRequest request =
                    make_request("chain-" + std::to_string(seed) + "-" + std::to_string(step), AssetClass::Server,
                                 "Acme", "CH-" + std::to_string(seed) + "-" + std::to_string(step));
                if (!live.empty() && random.coin()) {
                    // Replace a random live asset after retiring it.
                    const AssetId target = live[random.below(live.size())];
                    auto retired = registry.transition_lifecycle(context.session, context.session.advance(), target,
                                                                 std::nullopt, LifecycleState::Decommissioned);
                    if (!retired.has_value()) {
                        continue;
                    }
                    request.supersedes = target;
                    request.supersession_cause = ReplacementCause::Failure;
                }
                auto registered = registry.register_asset(context.session, context.session.advance(), request);
                AR_CHECK_MSG(registered.has_value(), registered.has_value() ? "" : registered.error().to_string());
                if (registered.has_value()) {
                    live.push_back(registered.value().id);
                    all_identities.insert(registered.value().id.to_compact_string());
                }
                continue;
            }
            const AssetId target = live[random.below(live.size())];
            const AssetId other = live[random.below(live.size())];
            auto linked = registry.link_replacement(context.session, context.session.advance(), target, std::nullopt,
                                                    other, ReplacementCause::Upgrade);
            if (linked.has_value()) {
                // A successful link is only possible when the predecessor was
                // terminal and the successor had no predecessor, which the
                // invariants check below re-verifies over the whole snapshot.
                AR_CHECK(!(target == other));
            }
        }

        const Snapshot snapshot = registry.snapshot();
        AR_CHECK(snapshot.size() == live.size());
        check_snapshot_invariants(snapshot);
        // Every lineage walk terminates, and no walk revisits a node.
        for (const AssetId& identifier : snapshot.ids()) {
            const auto chain = snapshot.lineage(identifier, limits::kMaxLineageDepth);
            AR_REQUIRE_OK(lineage, chain);
            std::set<std::string> seen;
            for (const AssetId& ancestor : lineage.predecessors) {
                AR_CHECK(seen.insert(ancestor.to_compact_string()).second);
            }
            // Successors and predecessors are mutually consistent.
            for (const AssetId& successor : lineage.successors) {
                const auto back = snapshot.predecessor_of(successor);
                AR_CHECK(back.has_value());
                if (back.has_value()) {
                    AR_CHECK(*back == identifier);
                }
            }
        }
    }
}

AR_TEST(property, random_durable_sequences_survive_reopen_intact) {
    TempDirectory directory("property-durable");
    const std::uint64_t seed = 20240607;
    SeededRandom random(seed);
    std::map<std::string, ModelAsset> model;
    std::vector<std::string> labels;

    auto context = open_registry(directory.path());
    for (int step = 0; step < 60; ++step) {
        if (labels.empty() || random.below(100) < 40) {
            const std::string label = "durable-prop-" + std::to_string(step);
            RegisterAssetRequest request = make_request(label, AssetClass::Server, "Acme",
                                                        "DP-" + std::to_string(step));
            auto registered = context.registry->register_asset(context.session, context.session.advance(), request);
            AR_CHECK(registered.has_value());
            if (registered.has_value()) {
                ModelAsset entry;
                entry.state = AssetState{request.lifecycle, request.installation};
                entry.metadata = request.metadata;
                model.emplace(label, entry);
                labels.push_back(label);
            }
            continue;
        }
        const std::string& label = labels[random.below(labels.size())];
        ModelAsset& entry = model[label];
        const AssetId identifier = asset_id_for(label);
        if (random.coin()) {
            MetadataPatch patch;
            patch.owner = owner("org.example.platform");
            patch.notes = "note-" + std::to_string(random.next());
            auto result = context.registry->update_metadata(context.session, context.session.advance(), identifier,
                                                           entry.revision, patch);
            if (result.has_value()) {
                entry.metadata.owner = owner("org.example.platform");
                entry.metadata.notes = patch.notes.value();
                entry.revision = result.value().revision;
            }
        } else {
            const InstallationState targets[] = {InstallationState::NotInstalled, InstallationState::Staged,
                                                 InstallationState::Installed};
            const InstallationState target = targets[random.below(3)];
            auto result = context.registry->transition_installation(context.session, context.session.advance(),
                                                                   identifier, entry.revision, target);
            if (result.has_value()) {
                entry.state.installation = target;
                entry.revision = result.value().revision;
            }
        }
    }
    const TransactionSequence committed = context.registry->published_sequence();
    AR_CHECK(context.registry->close().has_value());

    // Reopen and verify the model exactly.
    auto reopened = open_registry(directory.path());
    const Snapshot snapshot = reopened.registry->snapshot();
    AR_CHECK(snapshot.size() == model.size());
    AR_CHECK(snapshot.published_sequence() == committed);
    for (const auto& entry : model) {
        const auto view = snapshot.find(asset_id_for(entry.first));
        AR_REQUIRE_PRESENT(found, view);
        AR_CHECK(found.revision == entry.second.revision);
        AR_CHECK(found.state == entry.second.state);
        AR_CHECK(found.metadata.owner == entry.second.metadata.owner);
        AR_CHECK(found.metadata.notes == entry.second.metadata.notes);
        AR_CHECK(found.provenance->size() == entry.second.revision.value());
    }
    check_snapshot_invariants(snapshot);
    AR_CHECK(reopened.registry->close().has_value());
}

AR_TEST(property, every_seed_reports_its_stream_and_the_same_state) {
    // The generator itself must be reproducible: the same seed must produce the
    // same stream, or a reported seed would not be a replayable failure.
    SeededRandom first(4242);
    SeededRandom second(4242);
    for (int index = 0; index < 256; ++index) {
        AR_CHECK(first.next() == second.next());
    }
    SeededRandom other(4243);
    SeededRandom again(4242);
    bool differs = false;
    for (int index = 0; index < 64; ++index) {
        if (other.next() != again.next()) {
            differs = true;
            break;
        }
    }
    AR_CHECK(differs);
    asset_test::log_line("property streams are reproducible from their seeds; seeds used: 1,2,3,7,11,99,222,"
                         "3333,4242,12345,0xC0FFEE,0xDEADBEEF,20240607");
}
