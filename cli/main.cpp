// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// asset-registry: inspection and narrow mutation tooling.
//
// The CLI is a thin shell over the public API. It never reaches into persistence
// structures, and every mutation it offers goes through the same authority,
// revision, and idempotency checks a library consumer faces. Read commands open
// the store read-only and take no lock, so inspection cannot disturb a running
// writer.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <functional>
#include <limits>
#include <optional>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/asset_registry.hpp"

namespace {

using namespace asset_registry;

/// Parses a canonical non-negative decimal integer. Written locally because the
/// CLI is a consumer of the public API and must not depend on implementation
/// details of the library.
[[nodiscard]] std::optional<std::uint64_t> parse_canonical_unsigned(std::string_view text) {
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    if (text.size() > 1 && text.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return std::nullopt;
        }
        value = value * 10U + digit;
    }
    return value;
}

/// Escapes text for a JSON string body, using deterministic escape selection.
[[nodiscard]] std::string escape_json(std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(text.size() + 8);
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (byte < 0x20U) {
                    result += "\\u00";
                    result.push_back(kHex[(byte >> 4U) & 0x0FU]);
                    result.push_back(kHex[byte & 0x0FU]);
                } else {
                    result.push_back(raw);
                }
                break;
        }
    }
    return result;
}

int g_exit_code = 0;

void print_error(const Error& error) {
    std::fprintf(stderr, "error: %s\n", error.to_string().c_str());
    g_exit_code = 1;
}

void fail(std::string_view message) {
    std::fprintf(stderr, "error: %.*s\n", static_cast<int>(message.size()), message.data());
    g_exit_code = 2;
}

struct Arguments {
    std::string command;
    std::string directory;
    std::map<std::string, std::string> options;
    std::vector<std::string> positional;

    [[nodiscard]] std::optional<std::string> option(std::string_view name) const {
        const auto found = options.find(std::string(name));
        if (found == options.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    [[nodiscard]] std::string option_or(std::string_view name, std::string fallback) const {
        const auto found = option(name);
        return found.has_value() ? *found : std::move(fallback);
    }

    [[nodiscard]] bool flag(std::string_view name) const { return options.find(std::string(name)) != options.end(); }
};

void print_usage() {
    std::printf(
        "asset-registry: authoritative physical asset inventory (DCCP Tranche 1)\n"
        "\n"
        "usage: asset-registry <command> <store-directory> [options]\n"
        "\n"
        "read-only inspection (uses a read-only open, takes no store lock):\n"
        "  info                  policy, bounds, layout, retention, recovery report\n"
        "  list                  canonical identities in order\n"
        "  show <asset-id>       one record with its retained provenance\n"
        "  lineage <asset-id>    replacement chain to the origin and direct successors\n"
        "  verify                integrity audit, conflict audit, and lineage check\n"
        "  states                the lifecycle and installation transition tables\n"
        "  classes               the asset class tokens\n"
        "  export                canonical JSON document (--out <file> to write it)\n"
        "  limits                the configured bounds and their defaults\n"
        "\n"
        "mutation (requires --writer, and refuses exactly what the library refuses):\n"
        "  register <file>       register one asset from a canonical JSON record\n"
        "  set-metadata <id>     patch metadata (--name, --owner, --site, --notes,\n"
        "                        --label key=value, --remove-label key)\n"
        "  set-state <id>        transition state (--lifecycle, --installation)\n"
        "  attach <id>           attach a reference (--capability, --location, --rack,\n"
        "                        --external kind/id, --evidence)\n"
        "  detach <id>           detach a reference (same target options)\n"
        "  link-replacement <id> record that this asset supersedes --predecessor\n"
        "\n"
        "common options:\n"
        "  --writer <name>       writer identity; required for every mutation\n"
        "  --idempotency-key <k> makes a mutation retry-safe\n"
        "  --reason <text>       recorded in provenance\n"
        "  --expect-revision <n> or 'any'; refuses a stale mutation\n"
        "  --json                machine-readable output where supported\n"
        "  --limit <n>           maximum records listed (default 100)\n"
        "  --count <n>           number of imported assets to report (verify)\n"
        "\n"
        "exit codes: 0 success, 1 the operation was refused, 2 usage error\n");
}

[[nodiscard]] std::optional<Arguments> parse_arguments(int argc, char** argv) {
    Arguments arguments;
    arguments.command = argv[1];
    arguments.directory = argv[2];
    for (int index = 3; index < argc; ++index) {
        const std::string_view token(argv[index]);
        if (token.rfind("--", 0) != 0) {
            arguments.positional.emplace_back(token);
            continue;
        }
        const std::size_t equals = token.find('=');
        if (equals != std::string_view::npos) {
            arguments.options.emplace(std::string(token.substr(2, equals - 2)), std::string(token.substr(equals + 1)));
            continue;
        }
        const std::string name(token.substr(2));
        if (name == "json") {
            arguments.options.emplace(name, "true");
            continue;
        }
        if (index + 1 >= argc) {
            fail("option --" + name + " requires a value");
            return std::nullopt;
        }
        arguments.options.emplace(name, argv[index + 1]);
        ++index;
    }
    return arguments;
}

/// Opens the store read-only. A read-only open takes no lock, publishes no epoch,
/// and can run alongside a writer.
[[nodiscard]] std::shared_ptr<AssetRegistry> open_read_only(const std::string& directory, RecoveryReport& report) {
    StoreOpenOptions options;
    auto registry = AssetRegistry::open(directory, StoreOpenMode::ReadOnly, options, report);
    if (!registry) {
        print_error(registry.error());
        return nullptr;
    }
    return registry.value();
}

/// Opens the store for reading and writing and arms one writer. The writer's
/// sequence continues from the recorded high-water mark for that identity, so a
/// reissued command is recognised as a retry rather than applied twice.
[[nodiscard]] std::shared_ptr<AssetRegistry> open_writer_store(const std::string& directory,
                                                               RecoveryReport& report) {
    StoreOpenOptions options;
    auto registry = AssetRegistry::open(directory, StoreOpenMode::CreateIfMissing, options, report);
    if (!registry) {
        print_error(registry.error());
        return nullptr;
    }
    return registry.value();
}

[[nodiscard]] std::optional<AssetId> parse_identifier(std::string_view text) {
    const auto parsed = AssetId::parse(text);
    if (!parsed.has_value()) {
        fail("not a canonical asset identity (expected 8-4-4-4-12 lowercase hexadecimal): " + std::string(text));
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<RevisionExpectation> parse_expectation(const Arguments& arguments) {
    const auto value = arguments.option("expect-revision");
    if (!value.has_value() || *value == "any") {
        return RevisionExpectation{};
    }
    const auto parsed = AssetRevision::parse(*value);
    if (!parsed.has_value()) {
        fail("--expect-revision must be a canonical positive integer or 'any'");
        return std::nullopt;
    }
    return RevisionExpectation{*parsed};
}

/// Runs one mutation through the shared envelope so every mutating command has the
/// same authority, idempotency, and reporting behaviour.
void run_mutation(const Arguments& arguments,
                  const std::function<Outcome<MutationResult>(AssetRegistry&, WriterSession&,
                                                              const MutationEnvelope&)>& operation) {
    RecoveryReport report;
    auto registry = open_writer_store(arguments.directory, report);
    if (registry == nullptr) {
        return;
    }
    const std::string writer_name = arguments.option_or("writer", "");
    if (writer_name.empty()) {
        fail("mutations require --writer <name> so the change is attributed and fenced");
        return;
    }
    const auto writer_id = WriterId::create(writer_name);
    if (!writer_id.has_value()) {
        fail("--writer is not a valid writer identity");
        return;
    }
    auto session = registry->open_writer(*writer_id);
    if (!session) {
        print_error(session.error());
        return;
    }
    WriterSession armed = session.value();
    MutationEnvelope envelope = armed.advance(arguments.option("idempotency-key"), arguments.option_or("reason", ""));
    auto result = operation(*registry, armed, envelope);
    if (!result) {
        print_error(result.error());
        (void)registry->close();
        return;
    }
    std::printf("applied revision=%s sequence=%s%s\n", result.value().revision.to_string().c_str(),
                result.value().published_sequence.has_value()
                    ? result.value().published_sequence->to_string().c_str()
                    : "replayed",
                result.value().idempotent_replay ? " (replayed from the recorded outcome)" : "");
    (void)registry->close();
}

void command_info(const Arguments& arguments) {
    RecoveryReport report;
    auto registry = open_read_only(arguments.directory, report);
    if (registry == nullptr) {
        return;
    }
    const Snapshot snapshot = registry->snapshot();
    const InventorySummary summary = snapshot.summary();
    const StoreMetadata metadata = registry->store() != nullptr ? registry->store()->metadata() : StoreMetadata{};
    std::printf("store:              %s\n", std::filesystem::absolute(arguments.directory).string().c_str());
    std::printf("store format:       %s\n", metadata.format_version.to_string().c_str());
    std::printf("registry epoch:     %s\n", snapshot.epoch().to_string().c_str());
    std::printf("published sequence: %s\n", snapshot.published_sequence().to_string().c_str());
    std::printf("last writer:        %s\n", metadata.last_writer.empty() ? "(none)" : metadata.last_writer.c_str());
    std::printf("assets:             %llu\n", static_cast<unsigned long long>(summary.total_assets));
    std::printf("references:         %llu\n", static_cast<unsigned long long>(summary.total_references));
    std::printf("provenance steps:   %llu\n", static_cast<unsigned long long>(summary.total_provenance_steps));
    std::printf("estimated bytes:    %llu\n", static_cast<unsigned long long>(snapshot.estimated_bytes()));
    std::printf("last recovery:      %s\n", std::string(to_string(report.action)).c_str());
    if (!report.detail.empty()) {
        std::printf("recovery detail:    %s\n", report.detail.c_str());
    }
    for (const std::string& name : report.affected_files) {
        std::printf("affected file:      %s\n", name.c_str());
    }
    if (registry->store() != nullptr) {
        std::printf("retained generations: %s\n", registry->store()->retained_generations().size() == 0
                                                      ? "(none recorded)"
                                                      : registry->store()->retained_generations().back().c_str());
        std::printf("generations kept:   %u\n", registry->store()->retained_generation_count());
    }
    std::printf("policy fingerprint: %s\n", snapshot.policy().to_canonical_string().c_str());
    (void)registry->close();
}

void command_list(const Arguments& arguments) {
    RecoveryReport report;
    auto registry = open_read_only(arguments.directory, report);
    if (registry == nullptr) {
        return;
    }
    const Snapshot snapshot = registry->snapshot();
    std::size_t limit = 100;
    if (const auto value = arguments.option("limit"); value.has_value()) {
        const auto parsed = parse_canonical_unsigned(*value);
        if (!parsed.has_value()) {
            fail("--limit must be a canonical non-negative integer");
            (void)registry->close();
            return;
        }
        limit = static_cast<std::size_t>(*parsed);
    }
    std::size_t shown = 0;
    const std::vector<AssetView> records = snapshot.records();
    for (const AssetView& view : records) {
        if (shown >= limit) {
            std::printf("... %llu more record(s) not shown; raise --limit to see them\n",
                        static_cast<unsigned long long>(records.size() - shown));
            break;
        }
        if (arguments.flag("json")) {
            const std::string class_token(to_string(view.asset_class));
            const std::string lifecycle_token(to_string(view.state.lifecycle));
            const std::string installation_token(to_string(view.state.installation));
            std::printf("{\"asset_id\":\"%s\",\"display_name\":\"%s\",\"asset_class\":\"%s\","
                        "\"lifecycle\":\"%s\",\"installation\":\"%s\",\"revision\":%s,\"generation\":%s}\n",
                        view.id.to_string().c_str(), escape_json(view.metadata.display_name).c_str(),
                        class_token.c_str(), lifecycle_token.c_str(), installation_token.c_str(),
                        view.revision.to_string().c_str(), view.generation.to_string().c_str());
        } else {
            std::printf("%s  %-23.*s %-14.*s %-11.*s rev=%-4s gen=%s  %s\n", view.id.to_string().c_str(),
                        static_cast<int>(to_string(view.asset_class).size()), to_string(view.asset_class).data(),
                        static_cast<int>(to_string(view.state.lifecycle).size()),
                        to_string(view.state.lifecycle).data(),
                        static_cast<int>(to_string(view.state.installation).size()),
                        to_string(view.state.installation).data(), view.revision.to_string().c_str(),
                        view.generation.to_string().c_str(), view.metadata.display_name.c_str());
        }
        ++shown;
    }
    std::printf("%llu record(s); %zu shown\n", static_cast<unsigned long long>(records.size()), shown);
    (void)registry->close();
}

void command_show(const Arguments& arguments) {
    if (arguments.positional.empty()) {
        fail("show requires an asset identity");
        return;
    }
    const auto identifier = parse_identifier(arguments.positional.front());
    if (!identifier.has_value()) {
        return;
    }
    RecoveryReport report;
    auto registry = open_read_only(arguments.directory, report);
    if (registry == nullptr) {
        return;
    }
    const Snapshot snapshot = registry->snapshot();
    const auto view = snapshot.find(*identifier);
    if (!view.has_value()) {
        fail("asset is not present in this store: " + identifier->to_string());
        (void)registry->close();
        return;
    }
    std::printf("asset_id:        %s\n", view->id.to_string().c_str());
    std::printf("asset_class:     %.*s\n", static_cast<int>(to_string(view->asset_class).size()),
                to_string(view->asset_class).data());
    std::printf("generation:      %s\n", view->generation.to_string().c_str());
    std::printf("revision:        %s\n", view->revision.to_string().c_str());
    std::printf("last_sequence:   %s\n", view->last_sequence.to_string().c_str());
    std::printf("display_name:    %s\n", view->metadata.display_name.c_str());
    std::printf("owner:           %s\n",
                view->metadata.owner.has_value() ? view->metadata.owner->text().c_str() : "(unknown)");
    std::printf("site:            %s\n",
                view->metadata.site.has_value() ? view->metadata.site->text().c_str() : "(unknown)");
    std::printf("notes:           %s\n", view->metadata.notes.c_str());
    std::printf("manufacturer:    %s\n", view->serial_identity.manufacturer().name().c_str());
    std::printf("serial:          %s\n", view->serial_identity.serial().text().c_str());
    std::printf("model:           %s\n",
                view->serial_identity.model().has_value() ? view->serial_identity.model()->text().c_str()
                                                          : "(unknown)");
    std::printf("serial_key:      %s\n", view->serial_identity.canonical_key().c_str());
    std::printf("lifecycle:       %.*s\n", static_cast<int>(to_string(view->state.lifecycle).size()),
                to_string(view->state.lifecycle).data());
    std::printf("installation:    %.*s\n", static_cast<int>(to_string(view->state.installation).size()),
                to_string(view->state.installation).data());
    for (const auto& label : view->metadata.labels) {
        std::printf("label:           %s=%s\n", label.first.c_str(), label.second.c_str());
    }
    for (const Reference& reference : view->references) {
        std::printf("reference:       %s evidence=%.*s\n", reference.canonical().c_str(),
                    static_cast<int>(to_string(reference.evidence()).size()), to_string(reference.evidence()).data());
    }
    if (view->supersedes.has_value()) {
        std::printf("supersedes:      %s cause=%.*s\n", view->supersedes->predecessor.to_string().c_str(),
                    static_cast<int>(to_string(view->supersedes->cause).size()),
                    to_string(view->supersedes->cause).data());
        std::printf("supersede note:  %s\n", view->supersedes->note.c_str());
    }
    if (view->history != nullptr) {
        for (const AssetIncarnation& incarnation : *view->history) {
            std::printf("previous:        generation=%s serial=%s lifecycle=%.*s closed_at=%s\n",
                        incarnation.generation.to_string().c_str(), incarnation.serial_identity.to_string().c_str(),
                        static_cast<int>(to_string(incarnation.state.lifecycle).size()),
                        to_string(incarnation.state.lifecycle).data(), incarnation.closed_at.to_string().c_str());
        }
    }
    if (view->provenance != nullptr) {
        std::printf("provenance (%zu retained step(s)):\n", view->provenance->size());
        for (const ProvenanceStep& step : *view->provenance) {
            std::printf("  %s rev=%-4s %-24.*s %-10.*s %s %s\n", step.sequence.to_string().c_str(),
                        step.revision.to_string().c_str(), static_cast<int>(to_string(step.action).size()),
                        to_string(step.action).data(), static_cast<int>(to_string(step.origin).size()),
                        to_string(step.origin).data(),
                        step.recorded_at.has_value() ? step.recorded_at->to_rfc3339().c_str() : "(no timestamp)",
                        step.change.c_str());
        }
    }
    (void)registry->close();
}

void command_lineage(const Arguments& arguments) {
    if (arguments.positional.empty()) {
        fail("lineage requires an asset identity");
        return;
    }
    const auto identifier = parse_identifier(arguments.positional.front());
    if (!identifier.has_value()) {
        return;
    }
    RecoveryReport report;
    auto registry = open_read_only(arguments.directory, report);
    if (registry == nullptr) {
        return;
    }
    const Snapshot snapshot = registry->snapshot();
    auto chain = snapshot.lineage(*identifier);
    if (!chain) {
        print_error(chain.error());
        (void)registry->close();
        return;
    }
    std::printf("asset:        %s\n", identifier->to_string().c_str());
    std::printf("termination:  %.*s\n", static_cast<int>(to_string(chain.value().termination).size()),
                to_string(chain.value().termination).data());
    std::printf("predecessors (%zu, nearest first):\n", chain.value().predecessors.size());
    for (const AssetId& ancestor : chain.value().predecessors) {
        const auto view = snapshot.find(ancestor);
        std::printf("  %s  %s\n", ancestor.to_string().c_str(),
                    view.has_value() ? view->metadata.display_name.c_str() : "(not present in this store)");
    }
    std::printf("successors (%zu):\n", chain.value().successors.size());
    for (const AssetId& successor : chain.value().successors) {
        const auto view = snapshot.find(successor);
        std::printf("  %s  %s\n", successor.to_string().c_str(),
                    view.has_value() ? view->metadata.display_name.c_str() : "(not present in this store)");
    }
    if (chain.value().successors_truncated) {
        std::printf("note: the successor list was truncated at the configured fan-out bound\n");
    }
    (void)registry->close();
}

void command_verify(const Arguments& arguments) {
    RecoveryReport report;
    auto registry = open_read_only(arguments.directory, report);
    if (registry == nullptr) {
        return;
    }
    const Snapshot snapshot = registry->snapshot();
    const ConflictReport conflicts = snapshot.audit_conflicts();
    std::printf("store:                 %s\n", std::filesystem::absolute(arguments.directory).string().c_str());
    std::printf("recovery action:       %s\n", std::string(to_string(report.action)).c_str());
    std::printf("recovery modified:     %s\n", report.store_modified ? "yes" : "no");
    std::printf("content rolled back:   %s\n", report.content_rolled_back ? "yes" : "no");
    std::printf("epoch:                 %s\n", snapshot.epoch().to_string().c_str());
    std::printf("published sequence:    %s\n", snapshot.published_sequence().to_string().c_str());
    std::printf("assets:                %llu\n", static_cast<unsigned long long>(snapshot.size()));
    std::printf("duplicate serial keys: %zu\n", conflicts.duplicate_serial_keys.size());
    std::printf("model conflicts:       %zu\n", conflicts.model_conflicts.size());
    std::printf("dangling predecessors: %zu\n", conflicts.dangling_predecessors.size());
    std::printf("state inconsistencies: %zu\n", conflicts.state_inconsistencies.size());
    std::printf("supersession mismatch: %zu\n", conflicts.supersession_state_mismatches.size());

    std::size_t chain_failures = 0;
    for (const AssetId& identifier : snapshot.ids()) {
        const auto chain = snapshot.lineage(identifier, limits::kMaxLineageDepth);
        if (!chain) {
            std::printf("lineage failure:       %s: %s\n", identifier.to_string().c_str(),
                        chain.error().to_string().c_str());
            ++chain_failures;
        }
    }
    std::printf("lineage failures:      %zu\n", chain_failures);
    const bool clean = conflicts.clean() && chain_failures == 0;
    std::printf("result:                %s\n", clean ? "consistent" : "INCONSISTENT");
    if (!clean) {
        g_exit_code = 1;
    }
    (void)registry->close();
}

void command_states(const Arguments&) {
    std::printf("lifecycle transitions (from -> permitted next):\n");
    for (std::size_t index = 0; index < lifecycle_state_count(); ++index) {
        const LifecycleState from = lifecycle_states()[index];
        std::printf("  %-16.*s ->", static_cast<int>(to_string(from).size()), to_string(from).data());
        const std::vector<LifecycleState> successors = legal_successors(from);
        if (successors.empty()) {
            std::printf(" (terminal)");
        }
        for (const LifecycleState target : successors) {
            std::printf(" %.*s", static_cast<int>(to_string(target).size()), to_string(target).data());
        }
        std::printf("\n");
    }
    std::printf("installation transitions (from -> permitted next):\n");
    for (std::size_t index = 0; index < installation_state_count(); ++index) {
        const InstallationState from = installation_states()[index];
        std::printf("  %-16.*s ->", static_cast<int>(to_string(from).size()), to_string(from).data());
        const std::vector<InstallationState> successors = legal_successors(from);
        if (successors.empty()) {
            std::printf(" (terminal)");
        }
        for (const InstallationState target : successors) {
            std::printf(" %.*s", static_cast<int>(to_string(target).size()), to_string(target).data());
        }
        std::printf("\n");
    }
    std::printf("cross-dimension consistency rules:\n");
    const StateConsistencyRules& rules = default_consistency_rules();
    std::printf("  active requires installed:        %s\n", rules.require_installed_for_active ? "yes" : "no");
    std::printf("  maintenance requires installed:   %s\n", rules.require_installed_for_maintenance ? "yes" : "no");
    std::printf("  provisioned requires staged:      %s\n", rules.require_staged_for_provisioned ? "yes" : "no");
    std::printf("  superseded requires terminal:     %s\n", rules.require_terminal_for_superseded ? "yes" : "no");
    std::printf("terminal lifecycle states: decommissioned, disposed\n");
}

void command_classes(const Arguments&) {
    for (std::size_t index = 0; index < known_asset_class_count(); ++index) {
        const AssetClass value = *known_asset_classes()[index];
        std::printf("%-32.*s %.*s\n", static_cast<int>(to_string(value).size()), to_string(value).data(),
                    static_cast<int>(describe(value).size()), describe(value).data());
    }
    std::printf("%-32s %s\n", "unknown", "(rejected at registration; never stored)");
}

void command_limits(const Arguments&) {
    const RegistryLimits& limits = default_limits();
    std::printf("max_assets=%llu\n", static_cast<unsigned long long>(limits.max_assets));
    std::printf("max_references_per_asset=%u\n", limits.max_references_per_asset);
    std::printf("max_labels_per_asset=%u\n", limits.max_labels_per_asset);
    std::printf("max_label_value_bytes=%zu\n", limits.max_label_value_bytes);
    std::printf("max_provenance_per_asset=%u\n", limits.max_provenance_per_asset);
    std::printf("max_generations_per_asset=%u\n", limits.max_generations_per_asset);
    std::printf("max_display_name_bytes=%zu\n", limits.max_display_name_bytes);
    std::printf("max_document_bytes=%llu\n", static_cast<unsigned long long>(limits.max_document_bytes));
    std::printf("max_import_records=%u\n", limits.max_import_records);
    std::printf("max_export_records=%u\n", limits.max_export_records);
    std::printf("max_registry_bytes=%llu\n", static_cast<unsigned long long>(limits.max_registry_bytes));
    std::printf("max_idempotency_records_per_writer=%u\n", limits.max_idempotency_records_per_writer);
    std::printf("max_tracked_writers=%u\n", limits.max_tracked_writers);
    std::printf("max_orphan_temporaries=%u\n", limits.max_orphan_temporaries);
    std::printf("max_retained_generations=%u\n", limits.max_retained_generations);
    std::printf("max_traversal_depth=%u\n", limits.max_traversal_depth);
    std::printf("max_lineage_depth=%zu\n", limits::kMaxLineageDepth);
    std::printf("max_lineage_fanout=%zu\n", limits::kMaxLineageFanout);
}

void command_export(const Arguments& arguments) {
    RecoveryReport report;
    auto registry = open_read_only(arguments.directory, report);
    if (registry == nullptr) {
        return;
    }
    ExportOptions options;
    if (const auto format = arguments.option("format"); format.has_value()) {
        const auto parsed = export_format_from_token(*format);
        if (!parsed.has_value()) {
            fail("--format must be canonical_json, compact_json, or newline_delimited_json");
            (void)registry->close();
            return;
        }
        options.format = *parsed;
    }
    ExportReport export_report;
    auto document = registry->export_document(options, export_report);
    if (!document) {
        print_error(document.error());
        (void)registry->close();
        return;
    }
    if (const auto path = arguments.option("out"); path.has_value()) {
        std::FILE* file = std::fopen(path->c_str(), "wb");
        if (file == nullptr) {
            fail("cannot open output file: " + *path);
            (void)registry->close();
            return;
        }
        const std::string& text = document.value();
        const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
        if (written != text.size()) {
            fail("short write to output file: " + *path);
            (void)registry->close();
            return;
        }
        std::printf("wrote %llu record(s), %llu bytes to %s\n",
                    static_cast<unsigned long long>(export_report.records_exported),
                    static_cast<unsigned long long>(export_report.bytes_written), path->c_str());
    } else {
        std::fwrite(document.value().data(), 1, document.value().size(), stdout);
    }
    (void)registry->close();
}

void command_register(const Arguments& arguments) {
    if (arguments.positional.empty()) {
        fail("register requires a path to a canonical JSON asset record");
        return;
    }
    const std::string document = [&arguments] {
        std::FILE* file = std::fopen(arguments.positional.front().c_str(), "rb");
        if (file == nullptr) {
            return std::string();
        }
        std::string text;
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            text.append(buffer, read);
        }
        std::fclose(file);
        return text;
    }();
    if (document.empty()) {
        fail("cannot read the asset record file: " + arguments.positional.front());
        return;
    }
    const auto parsed = parse_import_asset_json(document, default_limits());
    if (!parsed) {
        print_error(parsed.error());
        return;
    }
    const std::string writer_name = arguments.option_or("writer", "");
    if (writer_name.empty()) {
        fail("mutations require --writer <name> so the change is attributed and fenced");
        return;
    }
    RecoveryReport report;
    auto registry = open_writer_store(arguments.directory, report);
    if (registry == nullptr) {
        return;
    }
    auto session = registry->open_writer(WriterId::create(writer_name).value());
    if (!session) {
        print_error(session.error());
        (void)registry->close();
        return;
    }
    WriterSession armed = session.value();
    MutationEnvelope envelope = armed.advance(arguments.option("idempotency-key"), arguments.option_or("reason", ""));

    RegisterAssetRequest request;
    request.id_mode = AssetIdMode::CallerSupplied;
    request.id = parsed.value().id;
    request.asset_class = parsed.value().asset_class;
    request.serial_identity = parsed.value().serial_identity;
    request.metadata = parsed.value().metadata;
    request.references = parsed.value().references;
    request.lifecycle = parsed.value().state.lifecycle;
    request.installation = parsed.value().state.installation;
    request.supersedes = parsed.value().supersedes;
    request.supersession_cause = parsed.value().supersession_cause;

    auto result = registry->register_asset(armed, envelope, request);
    if (!result) {
        print_error(result.error());
        (void)registry->close();
        return;
    }
    std::printf("registered %s revision=%s%s\n", result.value().id.to_string().c_str(),
                result.value().revision.to_string().c_str(),
                result.value().idempotent_replay ? " (replayed from the recorded outcome)" : "");
    (void)registry->close();
}

void command_set_metadata(const Arguments& arguments) {
    if (arguments.positional.empty()) {
        fail("set-metadata requires an asset identity");
        return;
    }
    const auto identifier = parse_identifier(arguments.positional.front());
    if (!identifier.has_value()) {
        return;
    }
    const auto expectation = parse_expectation(arguments);
    if (!expectation.has_value()) {
        return;
    }
    MetadataPatch patch;
    if (const auto name = arguments.option("name"); name.has_value()) {
        patch.display_name = *name;
    }
    if (const auto value = arguments.option("owner"); value.has_value()) {
        const auto parsed = OwnerId::create(*value);
        if (!parsed.has_value()) {
            fail("--owner is not a valid owner identifier");
            return;
        }
        patch.owner = *parsed;
    }
    if (arguments.flag("clear-owner")) {
        patch.clear_owner = true;
    }
    if (const auto value = arguments.option("site"); value.has_value()) {
        const auto parsed = LocationId::create(*value);
        if (!parsed.has_value()) {
            fail("--site is not a valid location identifier");
            return;
        }
        patch.site = *parsed;
    }
    if (const auto value = arguments.option("notes"); value.has_value()) {
        patch.notes = *value;
    }
    if (const auto value = arguments.option("label"); value.has_value()) {
        const std::size_t equals = value->find('=');
        if (equals == std::string::npos) {
            fail("--label must be written key=value");
            return;
        }
        patch.set_labels.emplace_back(value->substr(0, equals), value->substr(equals + 1));
    }
    if (const auto value = arguments.option("remove-label"); value.has_value()) {
        patch.remove_labels.push_back(*value);
    }
    run_mutation(arguments, [&](AssetRegistry& registry, WriterSession& session,
                                const MutationEnvelope& envelope) {
        return registry.update_metadata(session, envelope, *identifier, *expectation, patch);
    });
}

void command_set_state(const Arguments& arguments) {
    if (arguments.positional.empty()) {
        fail("set-state requires an asset identity");
        return;
    }
    const auto identifier = parse_identifier(arguments.positional.front());
    if (!identifier.has_value()) {
        return;
    }
    const auto expectation = parse_expectation(arguments);
    if (!expectation.has_value()) {
        return;
    }
    const auto lifecycle_text = arguments.option("lifecycle");
    const auto installation_text = arguments.option("installation");
    if (!lifecycle_text.has_value() && !installation_text.has_value()) {
        fail("set-state requires --lifecycle and/or --installation");
        return;
    }
    std::optional<LifecycleState> lifecycle;
    if (lifecycle_text.has_value()) {
        lifecycle = lifecycle_state_from_token(*lifecycle_text);
        if (!lifecycle.has_value()) {
            fail("--lifecycle is not a recognised state token");
            return;
        }
    }
    std::optional<InstallationState> installation;
    if (installation_text.has_value()) {
        installation = installation_state_from_token(*installation_text);
        if (!installation.has_value()) {
            fail("--installation is not a recognised state token");
            return;
        }
    }
    // Two transitions are two publications, not one: the CLI performs them in
    // order and reports the final revision, so the recorded history shows both
    // steps exactly as the API recorded them.
    RecoveryReport report;
    auto registry = open_writer_store(arguments.directory, report);
    if (registry == nullptr) {
        return;
    }
    const std::string writer_name = arguments.option_or("writer", "");
    if (writer_name.empty()) {
        fail("mutations require --writer <name> so the change is attributed and fenced");
        return;
    }
    auto session = registry->open_writer(WriterId::create(writer_name).value());
    if (!session) {
        print_error(session.error());
        (void)registry->close();
        return;
    }
    WriterSession armed = session.value();
    AssetRevision expected = expectation->has_value() ? **expectation : AssetRevision(0);
    bool applied = false;
    AssetRevision last{};
    if (installation.has_value()) {
        MutationEnvelope envelope = armed.advance(arguments.option("idempotency-key"), arguments.option_or("reason", ""));
        auto result = registry->transition_installation(armed, envelope, *identifier,
                                                       expectation->has_value() ? *expectation : RevisionExpectation{},
                                                       *installation);
        if (!result) {
            print_error(result.error());
            (void)registry->close();
            return;
        }
        last = result.value().revision;
        applied = true;
    }
    if (lifecycle.has_value()) {
        MutationEnvelope envelope = armed.advance(arguments.option("idempotency-key"), arguments.option_or("reason", ""));
        const RevisionExpectation next_expectation = applied ? RevisionExpectation{last} : *expectation;
        auto result = registry->transition_lifecycle(armed, envelope, *identifier, next_expectation, *lifecycle);
        if (!result) {
            print_error(result.error());
            (void)registry->close();
            return;
        }
        last = result.value().revision;
        applied = true;
    }
    (void)expected;
    std::printf("applied revision=%s\n", last.to_string().c_str());
    (void)registry->close();
}

/// Builds the reference a command named, or reports which target was missing.
[[nodiscard]] std::optional<Reference> reference_from(const Arguments& arguments, bool& reported) {
    reported = false;
    ReferenceEvidence evidence = ReferenceEvidence::Unverified;
    if (const auto value = arguments.option("evidence"); value.has_value()) {
        const auto parsed = reference_evidence_from_token(*value);
        if (!parsed.has_value()) {
            fail("--evidence must be unverified, verified, or stale");
            reported = true;
            return std::nullopt;
        }
        evidence = *parsed;
    }
    int targets = 0;
    std::optional<Reference> reference;
    if (const auto value = arguments.option("capability"); value.has_value()) {
        ++targets;
        const auto parsed = CapabilityReference::create(*value);
        if (!parsed.has_value()) {
            fail("--capability is not a valid namespace:path reference");
            reported = true;
            return std::nullopt;
        }
        reference = Reference::capability(*parsed, evidence);
    }
    if (const auto value = arguments.option("location"); value.has_value()) {
        ++targets;
        const auto parsed = LocationId::create(*value);
        if (!parsed.has_value()) {
            fail("--location is not a valid location identifier");
            reported = true;
            return std::nullopt;
        }
        reference = Reference::location(*parsed, evidence);
    }
    if (const auto value = arguments.option("rack"); value.has_value()) {
        ++targets;
        const auto parsed = RackId::create(*value);
        if (!parsed.has_value()) {
            fail("--rack is not a valid rack identifier");
            reported = true;
            return std::nullopt;
        }
        reference = Reference::rack(*parsed, evidence);
    }
    if (const auto value = arguments.option("external"); value.has_value()) {
        ++targets;
        const std::size_t slash = value->find('/');
        if (slash == std::string::npos) {
            fail("--external must be written kind/id");
            reported = true;
            return std::nullopt;
        }
        const auto parsed = ExternalObjectReference::create(value->substr(0, slash), value->substr(slash + 1));
        if (!parsed.has_value()) {
            fail("--external is not a valid external object reference");
            reported = true;
            return std::nullopt;
        }
        reference = Reference::external_object(*parsed, evidence);
    }
    if (targets != 1) {
        fail("exactly one of --capability, --location, --rack, or --external is required");
        reported = true;
        return std::nullopt;
    }
    return reference;
}

void command_attach(const Arguments& arguments) {
    if (arguments.positional.empty()) {
        fail("attach requires an asset identity");
        return;
    }
    const auto identifier = parse_identifier(arguments.positional.front());
    if (!identifier.has_value()) {
        return;
    }
    const auto expectation = parse_expectation(arguments);
    if (!expectation.has_value()) {
        return;
    }
    bool reported = false;
    const auto reference = reference_from(arguments, reported);
    if (reported || !reference.has_value()) {
        return;
    }
    run_mutation(arguments, [&](AssetRegistry& registry, WriterSession& session,
                                const MutationEnvelope& envelope) {
        return registry.attach_reference(session, envelope, *identifier, *expectation, *reference);
    });
}

void command_detach(const Arguments& arguments) {
    if (arguments.positional.empty()) {
        fail("detach requires an asset identity");
        return;
    }
    const auto identifier = parse_identifier(arguments.positional.front());
    if (!identifier.has_value()) {
        return;
    }
    const auto expectation = parse_expectation(arguments);
    if (!expectation.has_value()) {
        return;
    }
    bool reported = false;
    const auto reference = reference_from(arguments, reported);
    if (reported || !reference.has_value()) {
        return;
    }
    run_mutation(arguments, [&](AssetRegistry& registry, WriterSession& session,
                                const MutationEnvelope& envelope) {
        return registry.detach_reference(session, envelope, *identifier, *expectation, *reference);
    });
}

void command_link_replacement(const Arguments& arguments) {
    if (arguments.positional.empty()) {
        fail("link-replacement requires the successor asset identity");
        return;
    }
    const auto successor = parse_identifier(arguments.positional.front());
    if (!successor.has_value()) {
        return;
    }
    const auto expectation = parse_expectation(arguments);
    if (!expectation.has_value()) {
        return;
    }
    const auto predecessor_text = arguments.option("predecessor");
    if (!predecessor_text.has_value()) {
        fail("link-replacement requires --predecessor <asset-id>");
        return;
    }
    const auto predecessor = AssetId::parse(*predecessor_text);
    if (!predecessor.has_value()) {
        fail("--predecessor is not a canonical asset identity");
        return;
    }
    ReplacementCause cause = ReplacementCause::Unknown;
    if (const auto value = arguments.option("cause"); value.has_value()) {
        const auto parsed = replacement_cause_from_token(*value);
        if (!parsed.has_value()) {
            fail("--cause is not a recognised replacement cause token");
            return;
        }
        cause = *parsed;
    }
    const std::string note = arguments.option_or("note", "");
    run_mutation(arguments, [&](AssetRegistry& registry, WriterSession& session,
                                const MutationEnvelope& envelope) {
        return registry.link_replacement(session, envelope, *successor, *expectation, *predecessor, cause, note);
    });
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return argc < 2 ? 2 : 0;
    }
    const auto arguments = parse_arguments(argc, argv);
    if (!arguments.has_value()) {
        return 2;
    }
    const std::string& command = arguments->command;
    if (command == "info") {
        command_info(*arguments);
    } else if (command == "list") {
        command_list(*arguments);
    } else if (command == "show") {
        command_show(*arguments);
    } else if (command == "lineage") {
        command_lineage(*arguments);
    } else if (command == "verify") {
        command_verify(*arguments);
    } else if (command == "states") {
        command_states(*arguments);
    } else if (command == "classes") {
        command_classes(*arguments);
    } else if (command == "limits") {
        command_limits(*arguments);
    } else if (command == "export") {
        command_export(*arguments);
    } else if (command == "register") {
        command_register(*arguments);
    } else if (command == "set-metadata") {
        command_set_metadata(*arguments);
    } else if (command == "set-state") {
        command_set_state(*arguments);
    } else if (command == "attach") {
        command_attach(*arguments);
    } else if (command == "detach") {
        command_detach(*arguments);
    } else if (command == "link-replacement") {
        command_link_replacement(*arguments);
    } else {
        fail("unrecognised command: " + command + " (run with no arguments for usage)");
        return 2;
    }
    return g_exit_code;
}
