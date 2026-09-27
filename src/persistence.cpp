// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "asset_registry/persistence.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>
#include "asset_registry/text.hpp"
#include "crc32.hpp"
#include "fs_atomic.hpp"
#include "store_format.hpp"
#include "store_internal.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace asset_registry {

namespace {
constexpr std::string_view kLockFileName = "lock";
constexpr std::string_view kMetaFileName = "meta";
constexpr std::string_view kCurrentFileName = "CURRENT";
constexpr std::string_view kGenerationsDirName = "generations";
constexpr std::string_view kTmpDirName = "tmp";
constexpr std::string_view kGenerationPrefix = "gen-";
constexpr std::string_view kGenerationSuffix = ".dat";
constexpr std::size_t kGenerationDigits = 20;
constexpr std::size_t kMaxMetaBytes = 64U * 1024U;
constexpr std::size_t kMaxCurrentBytes = 4096U;

[[nodiscard]] std::string generation_file_name(TransactionSequence sequence) {
    std::string digits = sequence.to_string();
    std::string name(kGenerationPrefix);
    if (digits.size() < kGenerationDigits) {
        name.append(kGenerationDigits - digits.size(), '0');
    }
    name += digits;
    name += kGenerationSuffix;
    return name;
}

[[nodiscard]] std::optional<TransactionSequence> parse_generation_file_name(std::string_view name) {
    // The name is exactly "gen-" + twenty zero-padded decimal digits + ".dat".
    // Nothing else is a generation file, and a name of any other shape is ignored
    // rather than guessed at.
    if (name.size() != kGenerationPrefix.size() + kGenerationDigits + kGenerationSuffix.size()) {
        return std::nullopt;
    }
    if (name.substr(0, kGenerationPrefix.size()) != kGenerationPrefix) {
        return std::nullopt;
    }
    if (name.substr(name.size() - kGenerationSuffix.size()) != kGenerationSuffix) {
        return std::nullopt;
    }
    const std::string_view digits = name.substr(kGenerationPrefix.size(), kGenerationDigits);
    // The digits are zero padded by construction, so the leading zeros are removed
    // before the canonical integer parser runs: that parser exists to reject
    // non-canonical external input, and a padded generation name is canonical here.
    const std::size_t first_significant = digits.find_first_not_of('0');
    if (first_significant == std::string_view::npos) {
        // All zeros: sequence zero does not exist.
        return std::nullopt;
    }
    const auto parsed = parse_unsigned_decimal(digits.substr(first_significant));
    if (!parsed.has_value() || *parsed == 0) {
        return std::nullopt;
    }
    return TransactionSequence(*parsed);
}

/// Integrity marker for the metadata record. One definition, used by the encoder and
/// the decoder, so the two cannot disagree about where the checksummed region ends.
constexpr std::string_view kMetaIntegrityMarker = "\n#crc32=";

struct MetaRecord {
    std::uint64_t format_version = kStoreFormatVersion;
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
    std::uint64_t chain_floor = 0;
    std::uint64_t retained_generations = 0;
    std::string last_writer;
    std::string policy_fingerprint;
};

[[nodiscard]] std::string encode_meta(const MetaRecord& meta) {
    std::string document = "asset-registry-meta/1\n";
    document += "format_version=" + std::to_string(meta.format_version) + "\n";
    document += "epoch=" + std::to_string(meta.epoch) + "\n";
    document += "sequence=" + std::to_string(meta.sequence) + "\n";
    document += "chain_floor=" + std::to_string(meta.chain_floor) + "\n";
    document += "retained_generations=" + std::to_string(meta.retained_generations) + "\n";
    document += "last_writer=" + meta.last_writer + "\n";
    document += "policy_fingerprint=" + meta.policy_fingerprint + "\n";
    // Integrity convention, stated once because the encoder and the decoder must
    // agree on it byte for byte: the checksum covers every byte that precedes the
    // value field and nothing after it. The marker is therefore inside the
    // checksummed region, which lets the decoder locate the field and verify the
    // record in a single pass.
    document += kMetaIntegrityMarker;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08x", internal::crc32(std::string_view(document)));  // NOLINT
    document += hex;
    document += "\n";
    return document;
}

[[nodiscard]] Outcome<MetaRecord> decode_meta(std::string_view document) {
    constexpr std::string_view kHeader = "asset-registry-meta/1\n";
    constexpr std::string_view kMarker = kMetaIntegrityMarker;
    if (document.size() < kHeader.size() || document.substr(0, kHeader.size()) != kHeader) {
        return make_error(ErrorCode::StoreCorrupt, "store metadata header is not recognised");
    }
    const std::size_t marker = document.rfind(kMarker);
    if (marker == std::string_view::npos) {
        return make_error(ErrorCode::StoreCorrupt, "store metadata carries no integrity field");
    }
    // The value runs to the end of the line, not to the end of the file: the record
    // ends with a newline that is outside the integrity field.
    const std::size_t value_start = marker + kMarker.size();
    const std::size_t value_end = document.find('\n', value_start);
    const std::string_view stored_hex = document.substr(
        value_start, value_end == std::string_view::npos ? std::string_view::npos : value_end - value_start);
    if (stored_hex.size() != 8) {
        return make_error(ErrorCode::StoreCorrupt, "store metadata integrity field is malformed");
    }
    std::uint32_t stored = 0;
    for (const char character : stored_hex) {
        stored <<= 4U;
        if (character >= '0' && character <= '9') {
            stored |= static_cast<std::uint32_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            stored |= static_cast<std::uint32_t>(character - 'a' + 10);
        } else {
            return make_error(ErrorCode::StoreCorrupt, "store metadata integrity field is malformed");
        }
    }
    if (stored != internal::crc32(document.substr(0, marker + kMarker.size()))) {
        return make_error(ErrorCode::StoreCorrupt, "store metadata integrity check failed");
    }
    MetaRecord meta;
    std::unordered_set<std::string> seen;
    std::size_t offset = kHeader.size();
    // Field lines end at the marker, which is the blank line that introduces the
    // integrity field; the marker itself is not a field.
    const std::size_t body_end = marker;
    while (offset < body_end) {
        const std::size_t end = document.find('\n', offset);
        if (end == std::string_view::npos || end >= body_end) {
            break;
        }
        const std::string_view line = document.substr(offset, end - offset);
        offset = end + 1;
        if (line.empty()) {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return make_error(ErrorCode::StoreCorrupt, "store metadata carries a malformed line");
        }
        const std::string key(line.substr(0, equals));
        const std::string_view value = line.substr(equals + 1);
        if (!seen.insert(key).second) {
            return make_error(ErrorCode::StoreCorrupt, "store metadata repeats the field: " + key);
        }
        if (key == "last_writer") {
            meta.last_writer = std::string(value);
            continue;
        }
        if (key == "policy_fingerprint") {
            meta.policy_fingerprint = std::string(value);
            continue;
        }
        const auto parsed = parse_unsigned_decimal(value);
        if (!parsed.has_value()) {
            return make_error(ErrorCode::StoreCorrupt, "store metadata field is not a canonical integer: " + key);
        }
        if (key == "format_version") {
            meta.format_version = *parsed;
        } else if (key == "epoch") {
            meta.epoch = *parsed;
        } else if (key == "sequence") {
            meta.sequence = *parsed;
        } else if (key == "chain_floor") {
            meta.chain_floor = *parsed;
        } else if (key == "retained_generations") {
            meta.retained_generations = *parsed;
        } else {
            return make_error(ErrorCode::StoreCorrupt, "store metadata carries an unknown field: " + key);
        }
    }
    if (meta.format_version != kStoreFormatVersion) {
        return make_error(ErrorCode::StoreVersionUnsupported,
                          "store metadata format version " + std::to_string(meta.format_version) +
                              " is not supported by this build (expected " + std::to_string(kStoreFormatVersion) +
                              ")");
    }
    if (!meta.last_writer.empty() && !WriterId::create(meta.last_writer).has_value()) {
        return make_error(ErrorCode::StoreCorrupt, "store metadata records an invalid writer identity");
    }
    return meta;
}

[[nodiscard]] Outcome<TransactionSequence> decode_current(std::string_view document) {
    constexpr std::string_view kPrefix = "asset-registry-current/1\nsequence=";
    if (document.size() < kPrefix.size() || document.substr(0, kPrefix.size()) != kPrefix) {
        return make_error(ErrorCode::StoreCorrupt, "current-generation pointer header is not recognised");
    }
    const std::size_t newline = document.find('\n', kPrefix.size());
    const std::string_view number =
        document.substr(kPrefix.size(), newline == std::string_view::npos ? std::string_view::npos
                                                                         : newline - kPrefix.size());
    const auto parsed = parse_unsigned_decimal(number);
    if (!parsed.has_value() || *parsed == 0) {
        return make_error(ErrorCode::StoreCorrupt, "current-generation pointer does not name a generation");
    }
    return TransactionSequence(*parsed);
}

[[nodiscard]] std::string encode_current(TransactionSequence sequence) {
    std::string document = "asset-registry-current/1\nsequence=";
    document += sequence.to_string();
    document += "\n";
    return document;
}
#ifdef _WIN32
using LockHandle = HANDLE;
// INVALID_HANDLE_VALUE is a cast expression, so it cannot initialise a constexpr
// variable; a per-process constant is equivalent and avoids the cast in a
// constant expression.
const LockHandle kInvalidLock = INVALID_HANDLE_VALUE;
#else
using LockHandle = int;
constexpr LockHandle kInvalidLock = -1;
#endif

[[nodiscard]] Outcome<LockHandle> acquire_store_lock(const std::filesystem::path& lock_path) {
#ifdef _WIN32
    // A share mode of zero denies every other opener, so the store directory is
    // exclusive even before the advisory byte-range lock is taken.
    const HANDLE handle = ::CreateFileW(lock_path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
            return make_error(ErrorCode::StoreLocked, "another process holds the store lock: " + lock_path.string());
        }
        return make_error(ErrorCode::StoreOpenFailed,
                          "cannot open store lock file: " + lock_path.string() + ": " + internal::last_system_error());
    }
    OVERLAPPED overlapped{};
    if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) == 0) {
        const std::string detail = internal::last_system_error();
        ::CloseHandle(handle);
        return make_error(ErrorCode::StoreLocked,
                          "another process holds the store lock: " + lock_path.string() + ": " + detail);
    }
    return handle;
#else
    const int descriptor = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (descriptor < 0) {
        return make_error(ErrorCode::StoreOpenFailed,
                          "cannot open store lock file: " + lock_path.string() + ": " + internal::last_system_error());
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int saved = errno;
        ::close(descriptor);
        if (saved == EWOULDBLOCK || saved == EAGAIN) {
            return make_error(ErrorCode::StoreLocked, "another process holds the store lock: " + lock_path.string());
        }
        return make_error(ErrorCode::StoreOpenFailed,
                          "cannot lock store: " + lock_path.string() + ": " + std::strerror(saved));
    }
    return descriptor;
#endif
}

void release_store_lock(LockHandle handle) {
    if (handle == kInvalidLock) {
        return;
    }
#ifdef _WIN32
    OVERLAPPED overlapped{};
    ::UnlockFileEx(handle, 0, 1, 0, &overlapped);
    ::CloseHandle(handle);
#else
    ::flock(handle, LOCK_UN);
    ::close(handle);
#endif
}

struct LoadedGeneration {
    internal::StorePayload payload;
    std::uint32_t payload_crc = 0;
    TransactionSequence sequence;
};
/// Reads a generation file and verifies header, declared length, payload
/// integrity, header/payload sequence agreement, and payload decodability.

[[nodiscard]] Outcome<LoadedGeneration> load_generation(const std::filesystem::path& path,
                                                        const internal::DecodeLimits& decode_limits) {
    const Outcome<std::string> bytes = internal::read_file_bounded(path, decode_limits.max_bytes + 4096U);
    if (!bytes) {
        return bytes.error();
    }
    const auto header = internal::decode_generation_header(bytes.value());
    if (!header) {
        return header.error();
    }
    const std::string_view image = bytes.value();
    const auto payload_bytes = static_cast<std::size_t>(header.value().payload_bytes);
    const std::string_view payload = image.substr(internal::kHeaderBytes, payload_bytes);
    const std::string_view trailer = image.substr(internal::kHeaderBytes + payload_bytes, internal::kTrailerBytes);
    std::uint32_t stored_crc = 0;
    for (const char character : trailer) {
        stored_crc = (stored_crc << 8U) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(character));
    }
    const std::uint32_t computed_crc = internal::crc32(payload);
    if (stored_crc != computed_crc) {
        return make_error(ErrorCode::StoreIntegrityFailed,
                          "generation file payload checksum does not match its contents: " + path.string());
    }
    auto decoded = internal::decode_store_payload(payload, decode_limits);
    if (!decoded) {
        return decoded.error();
    }
    if (decoded.value().sequence.value() != header.value().sequence) {
        return make_error(ErrorCode::StoreIntegrityFailed,
                          "generation file header sequence does not match the payload sequence: " + path.string());
    }
    LoadedGeneration result;
    result.sequence = TransactionSequence(header.value().sequence);
    result.payload_crc = computed_crc;
    result.payload = std::move(decoded).value();
    return result;
}
/// Verifies that the authoritative generation's recorded ancestry is intact.
///
/// Each generation records the checksum of the generation it superseded, which
/// forms a hash chain. Walking it detects a CURRENT file that was edited by hand
/// to point at an older generation: no checksum inside a single generation file
/// can detect that, because each file is internally consistent.

[[nodiscard]] Outcome<void> verify_generation_chain(const std::filesystem::path& generations_directory,
                                                    const LoadedGeneration& head, std::uint64_t chain_floor,
                                                    std::size_t max_hops, const internal::DecodeLimits& decode_limits) {
    std::vector<TransactionSequence> visited;
    const LoadedGeneration* current = &head;
    std::optional<LoadedGeneration> owned;
    for (std::size_t hop = 0; hop < max_hops; ++hop) {
        if (std::find(visited.begin(), visited.end(), current->sequence) != visited.end()) {
            return make_error(ErrorCode::StoreCorrupt,
                              "generation ancestry revisits sequence " + current->sequence.to_string() +
                                  "; the store carries a cycle");
        }
        visited.push_back(current->sequence);
        const TransactionSequence predecessor = current->payload.predecessor_sequence;
        if (predecessor.is_zero() || predecessor.value() < chain_floor) {
            return Outcome<void>{};
        }
        auto previous = load_generation(generations_directory / generation_file_name(predecessor), decode_limits);
        if (!previous) {
            return make_error(ErrorCode::StoreIntegrityFailed,
                              "generation ancestry is broken at sequence " + predecessor.to_string() + ": " +
                                  previous.error().message());
        }
        if (previous.value().payload_crc != current->payload.predecessor_crc) {
            return make_error(ErrorCode::StoreIntegrityFailed,
                              "generation " + current->sequence.to_string() +
                                  " records a predecessor checksum that does not match generation " +
                                  predecessor.to_string());
        }
        owned = std::move(previous).value();
        current = &owned.value();
    }
    return make_error(ErrorCode::StoreIntegrityFailed,
                      "generation ancestry exceeds the retention bound; the store layout is inconsistent");
}
/// Reports the first stored bound that exceeds the configured bound. An empty
/// string means every stored bound is within configuration.

[[nodiscard]] std::string describe_limits_mismatch(const RegistryLimits& stored, const RegistryLimits& configured) {
    struct BoundCheck {
        const char* name;
        std::uint64_t stored_value;
        std::uint64_t configured_value;
    };
    const BoundCheck checks[] = {
        {"max_assets", stored.max_assets, configured.max_assets},
        {"max_references_per_asset", stored.max_references_per_asset, configured.max_references_per_asset},
        {"max_labels_per_asset", stored.max_labels_per_asset, configured.max_labels_per_asset},
        {"max_label_value_bytes", stored.max_label_value_bytes, configured.max_label_value_bytes},
        {"max_provenance_per_asset", stored.max_provenance_per_asset, configured.max_provenance_per_asset},
        {"max_generations_per_asset", stored.max_generations_per_asset, configured.max_generations_per_asset},
        {"max_display_name_bytes", stored.max_display_name_bytes, configured.max_display_name_bytes},
        {"max_import_records", stored.max_import_records, configured.max_import_records},
        {"max_export_records", stored.max_export_records, configured.max_export_records},
        {"max_registry_bytes", stored.max_registry_bytes, configured.max_registry_bytes},
        {"max_idempotency_records_per_writer", stored.max_idempotency_records_per_writer,
         configured.max_idempotency_records_per_writer},
        {"max_tracked_writers", stored.max_tracked_writers, configured.max_tracked_writers},
    };
    for (const BoundCheck& bound : checks) {
        if (bound.stored_value > bound.configured_value) {
            std::string message(bound.name);
            message += " (store holds ";
            message += std::to_string(bound.stored_value);
            message += ", configured bound is ";
            message += std::to_string(bound.configured_value);
            message += ")";
            return message;
        }
    }
    return std::string();
}
}  // namespace

struct Store::Impl {
    std::filesystem::path root;
    std::filesystem::path generations;
    std::filesystem::path tmp;
    StoreOpenMode mode = StoreOpenMode::ReadWrite;
    StoreOpenOptions options;
    RecoveryReport report;
    MetaRecord meta;
    RegistryLimits stored_limits = default_limits();
    mutable std::mutex mutex;
    bool is_open = false;
    std::vector<char> authoritative_payload;
    std::uint32_t authoritative_crc = 0;
    TransactionSequence authoritative_sequence;
    StoreLoadedState loaded;
    struct Grant {
        AuthorityGeneration generation;
        RegistryEpoch epoch;
        std::optional<Timestamp> expires_at;
        bool revoked = false;
        std::string reason;
    };
    std::map<std::string, Grant> grants;
    AuthorityGeneration next_grant{1};
    LockHandle lock_handle = kInvalidLock;
    [[nodiscard]] bool read_only() const noexcept { return mode == StoreOpenMode::ReadOnly; }
};
Store::Store() : impl_(std::make_unique<Impl>()) {}
Store::~Store() {
    if (impl_ != nullptr) {
        (void)close();
    }
}

Outcome<std::shared_ptr<Store>> Store::open(const std::filesystem::path& directory, StoreOpenMode mode,
                                            const StoreOpenOptions& options, RecoveryReport& report) {
    const auto limits_check = validate_limits(options.limits);
    if (limits_check.has_value()) {
        return make_error(ErrorCode::InvalidLimits, "store bounds are not usable: " + *limits_check);
    }
    if (directory.empty()) {
        return make_error(ErrorCode::InvalidInput, "store directory path must not be empty");
    }
    auto store = std::shared_ptr<Store>(new Store());
    Store& self = *store;
    Store::Impl& impl = *self.impl_;
    impl.options = options;
    impl.mode = mode;
    impl.root = directory;
    impl.generations = directory / std::string(kGenerationsDirName);
    impl.tmp = directory / std::string(kTmpDirName);
    impl.stored_limits = options.limits;
    impl.report = RecoveryReport{};
    if (mode == StoreOpenMode::ReadOnly) {
        std::error_code error;
        const bool exists = std::filesystem::exists(directory, error);
        if (error || !exists) {
            return make_error(ErrorCode::StoreNotFound, "store directory does not exist: " + directory.string());
        }
        if (!internal::is_regular_file(directory / std::string(kMetaFileName))) {
            return make_error(ErrorCode::StoreNotFound, "directory holds no asset registry store: " + directory.string());
        }
    } else {
        const Outcome<void> created = internal::ensure_directory(directory);
        if (!created) {
            return created.error();
        }
        const Outcome<LockHandle> lock = acquire_store_lock(directory / std::string(kLockFileName));
        if (!lock) {
            return lock.error();
        }
        impl.lock_handle = lock.value();
    }
    const Outcome<void> generations_ready = internal::ensure_directory(impl.generations);
    if (!generations_ready) {
        return generations_ready.error();
    }
    if (mode != StoreOpenMode::ReadOnly) {
        const Outcome<void> tmp_ready = internal::ensure_directory(impl.tmp);
        if (!tmp_ready) {
            return tmp_ready.error();
        }
    }
    const std::filesystem::path meta_path = directory / std::string(kMetaFileName);
    MetaRecord meta;
    const bool meta_exists = internal::is_regular_file(meta_path);
    if (meta_exists) {
        const Outcome<std::string> raw = internal::read_file_bounded(meta_path, kMaxMetaBytes);
        if (!raw) {
            return raw.error();
        }
        const auto decoded = decode_meta(raw.value());
        if (!decoded) {
            return decoded.error();
        }
        meta = decoded.value();
        if (!meta.policy_fingerprint.empty() && meta.policy_fingerprint != options.policy.to_canonical_string() &&
            options.require_policy_match) {
            return make_error(ErrorCode::InvalidConfiguration,
                              "stored policy fingerprint does not match the configured policy: the store was "
                              "written under different rules");
        }
    }
    impl.meta = meta;
    const Outcome<std::vector<std::string>> entries = internal::list_files(impl.generations);
    if (!entries) {
        return entries.error();
    }
    std::vector<TransactionSequence> discovered;
    for (const std::string& name : entries.value()) {
        const auto parsed = parse_generation_file_name(name);
        if (parsed.has_value()) {
            discovered.push_back(*parsed);
        }
    }
    std::sort(discovered.begin(), discovered.end());
    if (!meta_exists && discovered.empty()) {
        // A directory that holds neither metadata nor generations is either a
        // fresh location or a directory that was never a store.
        if (mode == StoreOpenMode::ReadOnly) {
            return make_error(ErrorCode::StoreNotFound, "directory holds no asset registry store: " + directory.string());
        }
        impl.report.action = RecoveryAction::Initialised;
        impl.report.store_modified = true;
        impl.report.detail = "no existing store was found; a new store was initialised";
        impl.loaded = StoreLoadedState{};
        impl.meta.epoch = meta.epoch + 1;
        impl.meta.sequence = 0;
        impl.meta.chain_floor = 0;
        impl.meta.retained_generations = 0;
        impl.meta.last_writer = options.initial_writer;
        impl.meta.policy_fingerprint = options.policy.to_canonical_string();
        const Outcome<void> written = internal::write_file_flushed(meta_path, encode_meta(impl.meta));
        if (!written) {
            return written.error();
        }
        const Outcome<void> pointer_written = internal::write_file_flushed(
            directory / std::string(kCurrentFileName), encode_current(TransactionSequence(0)));
        if (!pointer_written) {
            return pointer_written.error();
        }
        const Outcome<void> synced = internal::sync_directory(directory);
        if (!synced) {
            return synced.error();
        }
        impl.report.epoch = RegistryEpoch(impl.meta.epoch);
        report = impl.report;
        impl.is_open = true;
        return store;
    }
    if (discovered.empty()) {
        // A store whose metadata was accepted but that holds no generation yet is a
        // valid empty store: the first commit is what creates a generation. The
        // transaction sequence recorded in the metadata is authoritative here, so a
        // store that lost its generation files reports the sequence it had rather
        // than restarting from zero and reissuing identities.
        impl.report.action = RecoveryAction::None;
        impl.report.detail = "store holds metadata only; the inventory is empty";
        impl.loaded = StoreLoadedState{};
        impl.loaded.sequence = TransactionSequence(meta.sequence);
        impl.loaded.epoch = RegistryEpoch(meta.epoch);
        impl.loaded.last_writer = meta.last_writer;
        impl.authoritative_sequence = TransactionSequence(meta.sequence);
    } else {
        const std::filesystem::path current_path = directory / std::string(kCurrentFileName);
        std::optional<TransactionSequence> named;
        std::string current_problem = "the current-generation pointer is absent";
        if (internal::is_regular_file(current_path)) {
            const Outcome<std::string> raw = internal::read_file_bounded(current_path, kMaxCurrentBytes);
            if (raw) {
                const auto decoded = decode_current(raw.value());
                if (decoded) {
                    named = decoded.value();
                } else {
                    current_problem = decoded.error().message();
                }
            } else {
                current_problem = raw.error().message();
            }
        }
        std::optional<TransactionSequence> authoritative_pointer;
        if (named.has_value() && std::binary_search(discovered.begin(), discovered.end(), *named)) {
            authoritative_pointer = named;
        }
        internal::DecodeLimits probe_limits;
        probe_limits.limits = options.limits;
        probe_limits.max_bytes = options.limits.max_document_bytes + internal::kHeaderBytes + internal::kTrailerBytes;
        probe_limits.max_depth = options.limits.max_traversal_depth;
        std::vector<TransactionSequence> candidates(discovered.rbegin(), discovered.rend());
        std::optional<LoadedGeneration> chosen;
        std::vector<std::string> rejected;
        for (const TransactionSequence candidate : candidates) {
            const std::filesystem::path path = impl.generations / generation_file_name(candidate);
            auto loaded = load_generation(path, probe_limits);
            if (loaded) {
                chosen = std::move(loaded).value();
                break;
            }
            rejected.push_back(path.filename().string());
            if (named.has_value() && candidate == *named) {
                current_problem = loaded.error().message();
            }
        }
        if (!chosen.has_value()) {
            return make_error(ErrorCode::StoreRecoveryImpossible,
                              "no generation file in " + impl.generations.string() +
                                  " passed integrity validation; the last reported problem was: " + current_problem);
        }
        // The decoded bounds must not exceed the configured bounds, otherwise the
        // store would open with a state its own configuration says is too large.
        const std::string mismatch = describe_limits_mismatch(chosen->payload.limits, options.limits);
        if (!mismatch.empty()) {
            return make_error(ErrorCode::StoreIntegrityFailed,
                              "store holds a bound larger than this configuration permits: " + mismatch);
        }
        impl.stored_limits = chosen->payload.limits;
        if (authoritative_pointer.has_value() && *authoritative_pointer == chosen->sequence && rejected.empty()) {
            impl.report.action = RecoveryAction::None;
            impl.report.detail = "the current-generation pointer named a generation that passed every check";
        } else if (!authoritative_pointer.has_value()) {
            impl.report.action = RecoveryAction::RolledBackToLastValid;
            impl.report.detail = "the current-generation pointer could not be used (" + current_problem +
                                 "); the newest valid generation was adopted";
            impl.report.content_rolled_back = false;
            impl.report.store_modified = mode != StoreOpenMode::ReadOnly && options.allow_recovery;
        } else if (!rejected.empty() && *authoritative_pointer == chosen->sequence) {
            impl.report.action = RecoveryAction::DamagedGenerationQuarantined;
            impl.report.detail = "a generation newer than the authoritative one failed validation: " +
                                 current_problem;
            impl.report.store_modified = mode != StoreOpenMode::ReadOnly && options.allow_recovery;
        } else {
            impl.report.action = RecoveryAction::RolledBackToLastValid;
            impl.report.detail = "the authoritative generation could not be trusted (" + current_problem +
                                 "); the store was recovered to the newest valid generation";
            impl.report.content_rolled_back = true;
            impl.report.store_modified = mode != StoreOpenMode::ReadOnly && options.allow_recovery;
        }
        if (impl.report.content_rolled_back && !options.allow_recovery) {
            return make_error(ErrorCode::StoreIntegrityFailed,
                              "the authoritative generation is damaged and recovery is disabled: " + current_problem);
        }
        // Chain continuity: the authoritative generation names its predecessor's
        // checksum. Verifying the chain detects a CURRENT file that was edited by
        // hand to point at an older generation, which no checksum inside a single
        // generation file can detect.
        if (options.deep_verify_chain) {
            // The pointer and the metadata both name the generation that was committed last, and a
            // commit publishes both. An authoritative generation that is not the one they name was
            // therefore chosen by recovery rather than recorded by a commit: either a file was
            // damaged, or CURRENT was edited by hand to point somewhere else. Every generation file
            // is internally consistent, so comparing the adopted generation against what was
            // recorded is the only check that can tell the difference, and the store refuses rather
            // than publishing an inventory it cannot account for.
            const bool pointer_agrees = named.has_value() && named.value() == chosen->sequence;
            const bool metadata_agrees = !meta_exists || meta.sequence == chosen->sequence.value();
            if (!pointer_agrees || !metadata_agrees) {
                return make_error(ErrorCode::StoreIntegrityFailed,
                                  "generation ancestry was not accepted: the adopted generation " +
                                      chosen->sequence.to_string() +
                                      " is not the one the CURRENT pointer and the store metadata record");
            }
            const std::size_t max_hops = static_cast<std::size_t>(impl.stored_limits.max_retained_generations) + 2;
            const Outcome<void> chain = verify_generation_chain(impl.generations, chosen.value(), meta.chain_floor,
                                                                max_hops, probe_limits);
            if (!chain) {
                return chain.error();
            }
        }
        impl.loaded.sequence = chosen->sequence;
        impl.loaded.epoch = chosen->payload.epoch;
        impl.loaded.last_writer = chosen->payload.last_writer;
        impl.loaded.records = std::move(chosen->payload.records);
        impl.loaded.writers = std::move(chosen->payload.writers);
        impl.authoritative_sequence = chosen->sequence;
        impl.authoritative_crc = chosen->payload_crc;
        std::sort(rejected.begin(), rejected.end());
        impl.report.affected_files = rejected;
        impl.report.previous_sequence = named.value_or(TransactionSequence(0));
        impl.report.current_sequence = chosen->sequence;
    }
    if (mode != StoreOpenMode::ReadOnly) {
        // Note: the assignment below happens after chosen is moved from; the
        // authoritative pointer is republished from the adopted sequence.
        impl.meta.epoch = impl.meta.epoch + 1;
        impl.meta.sequence = impl.loaded.sequence.value();
        if (impl.meta.chain_floor == 0) {
            impl.meta.chain_floor = impl.authoritative_sequence.value();
        }
        const Outcome<void> written = internal::write_file_flushed(meta_path, encode_meta(impl.meta));
        if (!written) {
            return written.error();
        }
        if (impl.report.action != RecoveryAction::None) {
            const Outcome<void> pointer_written = internal::write_file_flushed(
                directory / std::string(kCurrentFileName), encode_current(impl.authoritative_sequence));
            if (!pointer_written) {
                return pointer_written.error();
            }
        }
        const Outcome<void> synced = internal::sync_directory(directory);
        if (!synced) {
            return synced.error();
        }
        if (options.clean_orphan_temporaries) {
            const Outcome<std::vector<std::string>> staged = internal::list_files(impl.tmp);
            if (staged) {
                std::size_t removed = 0;
                for (const std::string& name : staged.value()) {
                    const Outcome<void> gone = internal::remove_file(impl.tmp / name);
                    if (gone) {
                        ++removed;
                    }
                }
                if (removed > 0) {
                    impl.report.detail += "; removed " + std::to_string(removed) +
                                          " staging file(s) left by an interrupted commit";
                    impl.report.store_modified = true;
                }
            }
        }
    }
    impl.report.epoch = RegistryEpoch(impl.meta.epoch);
    report = impl.report;
    impl.is_open = true;
    return store;
}

bool Store::is_open() const noexcept {
    return impl_ != nullptr && impl_->is_open;
}

bool Store::is_read_only() const noexcept {
    return impl_ == nullptr || impl_->read_only();
}
const std::filesystem::path& Store::directory() const noexcept {
    static const std::filesystem::path kEmpty;
    return impl_ == nullptr ? kEmpty : impl_->root;
}
StoreMetadata Store::metadata() const {
    StoreMetadata result;
    if (impl_ == nullptr) {
        return result;
    }
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    result.format_version = StoreFormatVersion(static_cast<std::uint32_t>(impl_->meta.format_version));
    result.epoch = RegistryEpoch(impl_->meta.epoch);
    result.sequence = TransactionSequence(impl_->meta.sequence);
    result.last_writer = impl_->meta.last_writer;
    result.policy_fingerprint = impl_->meta.policy_fingerprint;
    result.retained_generations = static_cast<std::uint32_t>(impl_->meta.retained_generations);
    return result;
}
const RecoveryReport& Store::recovery() const noexcept {
    static const RecoveryReport kEmpty;
    return impl_ == nullptr ? kEmpty : impl_->report;
}
const RegistryPolicy& Store::policy() const noexcept {
    static const RegistryPolicy kDefault = default_policy();
    return impl_ == nullptr ? kDefault : impl_->options.policy;
}
const RegistryLimits& Store::limits() const noexcept {
    static const RegistryLimits kDefault = default_limits();
    if (impl_ == nullptr) {
        return kDefault;
    }
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->stored_limits;
}
RegistryEpoch Store::epoch() const noexcept {
    if (impl_ == nullptr) {
        return RegistryEpoch(0);
    }
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    return RegistryEpoch(impl_->meta.epoch);
}

std::uint32_t Store::retained_generation_count() const {
    if (impl_ == nullptr) {
        return 0;
    }
    const Outcome<std::vector<std::string>> names = internal::list_files(impl_->generations);
    if (!names) {
        return 0;
    }
    std::uint32_t count = 0;
    for (const std::string& name : names.value()) {
        if (parse_generation_file_name(name).has_value()) {
            ++count;
        }
    }
    return count;
}

std::vector<std::string> Store::retained_generations() const {
    std::vector<std::string> result;
    if (impl_ == nullptr) {
        return result;
    }
    const Outcome<std::vector<std::string>> names = internal::list_files(impl_->generations);
    if (!names) {
        return result;
    }
    for (const std::string& name : names.value()) {
        if (parse_generation_file_name(name).has_value()) {
            result.push_back(name);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

Outcome<AuthorityToken> Store::grant(const WriterId& writer, const AuthorityOptions& options) {
    if (impl_ == nullptr || !impl_->is_open) {
        return make_error(ErrorCode::RegistryClosed, "cannot mint authority on a closed store");
    }
    if (writer.empty()) {
        return make_error(ErrorCode::MalformedWriterId, "cannot mint authority for an empty writer identity");
    }
    if (options.reason.size() > limits::kMaxReasonBytes || validate_utf8(options.reason, true) != Utf8Status::Valid) {
        return make_error(ErrorCode::MalformedText, "authority grant reason violates text rules");
    }
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->is_open) {
        return make_error(ErrorCode::RegistryClosed, "the store was closed while authority was being minted");
    }
    if (impl_->read_only()) {
        return make_error(ErrorCode::AuthorityRevoked, "a read-only store mints no mutation authority");
    }
    AuthorityToken token;
    token.writer_ = writer;
    token.generation_ = impl_->next_grant;
    token.epoch_ = RegistryEpoch(impl_->meta.epoch);
    token.reason_ = options.reason;
    if (options.ttl_nanos.has_value()) {
        if (*options.ttl_nanos <= 0) {
            return make_error(ErrorCode::InvalidInput, "authority lifetime must be a positive duration");
        }
        const Timestamp now = SystemClock{}.now();
        const auto expiry = Timestamp::create(now.unix_nanos() + *options.ttl_nanos);
        if (!expiry.has_value()) {
            return make_error(ErrorCode::InvalidInput, "authority lifetime extends beyond the representable range");
        }
        token.expires_at_ = *expiry;
    }
    Impl::Grant grant_record;
    grant_record.generation = token.generation_;
    grant_record.epoch = token.epoch_;
    grant_record.expires_at = token.expires_at_;
    grant_record.reason = options.reason;
    impl_->grants[writer.text() + "#" + token.generation_.to_string()] = grant_record;
    const auto next = impl_->next_grant.next();
    if (!next.has_value()) {
        impl_->grants.erase(writer.text() + "#" + token.generation_.to_string());
        return make_error(ErrorCode::CapacityExceeded, "authority grant counter is exhausted for this store epoch");
    }
    impl_->next_grant = *next;
    return token;
}

Outcome<void> Store::revoke(const AuthorityToken& token) {
    if (impl_ == nullptr) {
        return make_error(ErrorCode::RegistryClosed, "cannot revoke authority on a closed store");
    }
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->grants.find(token.writer().text() + "#" + token.generation().to_string());
    if (found != impl_->grants.end()) {
        impl_->grants.erase(found);
    }
    // Revoking an unknown or already-revoked grant is a no-op: the grant is not
    // live, so the caller's intent is already satisfied.
    return Outcome<void>{};
}

bool Store::is_token_live(const AuthorityToken& token) const {
    if (impl_ == nullptr || !impl_->is_open) {
        return false;
    }
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->is_open || impl_->read_only() || !token.is_present()) {
        return false;
    }
    if (token.epoch().value() != impl_->meta.epoch) {
        return false;
    }
    const auto found = impl_->grants.find(token.writer().text() + "#" + token.generation().to_string());
    if (found == impl_->grants.end() || found->second.revoked) {
        return false;
    }
    if (found->second.expires_at.has_value() &&
        SystemClock{}.now().unix_nanos() > found->second.expires_at->unix_nanos()) {
        return false;
    }
    return true;
}

std::optional<MutationSequence> Store::writer_high_water(const WriterId& writer) const {
    if (impl_ == nullptr) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> guard(impl_->mutex);
    for (const internal::StoredWriter& stored : impl_->loaded.writers) {
        if (stored.writer == writer) {
            return stored.high_water;
        }
    }
    return std::nullopt;
}

Outcome<void> Store::close() {
    if (impl_ == nullptr) {
        return Outcome<void>{};
    }
    LockHandle handle = kInvalidLock;
    bool was_open = false;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        was_open = impl_->is_open;
        impl_->is_open = false;
        // Every outstanding grant is dropped on close, so a token held across a
        // close can never publish.
        impl_->grants.clear();
        handle = impl_->lock_handle;
        impl_->lock_handle = kInvalidLock;
    }
    release_store_lock(handle);
    if (!was_open) {
        return Outcome<void>{};
    }
    return Outcome<void>{};
}

Outcome<TransactionSequence> StoreAccess::commit(Store& store, internal::StorePayload& payload,
                                                 const WriterId& writer) {
    Store::Impl& impl = *store.impl_;
    if (!impl.is_open) {
        return make_error(ErrorCode::RegistryClosed, "cannot commit to a closed store");
    }
    std::lock_guard<std::mutex> guard(impl.mutex);
    if (!impl.is_open) {
        return make_error(ErrorCode::RegistryClosed, "the store was closed while a commit was in progress");
    }
    if (impl.read_only()) {
        return make_error(ErrorCode::AuthorityRevoked, "cannot commit to a store opened read-only");
    }
    const std::uint64_t next_value = impl.meta.sequence + 1;
    if (next_value <= impl.meta.sequence) {
        return make_error(ErrorCode::CapacityExceeded, "transaction sequence counter is exhausted");
    }
    const TransactionSequence next(next_value);
    if (!impl.authoritative_sequence.is_zero() && !(impl.authoritative_sequence < next)) {
        return make_error(ErrorCode::InternalInvariantViolation,
                          "reserved transaction sequence does not advance past the published sequence");
    }
    payload.sequence = next;
    payload.epoch = RegistryEpoch(impl.meta.epoch);
    payload.last_writer = writer.text();
    payload.predecessor_sequence = impl.authoritative_sequence;
    payload.predecessor_crc = impl.authoritative_crc;
    const std::uint64_t image_bound =
        impl.stored_limits.max_document_bytes + internal::kHeaderBytes + internal::kTrailerBytes;
    auto encoded = internal::encode_store_payload(payload, image_bound);
    if (!encoded) {
        return encoded.error();
    }
    internal::GenerationHeader header;
    header.format_version = kStoreFormatVersion;
    header.schema_version = kPayloadSchemaVersion;
    header.sequence = next.value();
    header.epoch = payload.epoch.value();
    header.payload_bytes = encoded.value().size();
    std::string image = internal::encode_generation_header(header);
    image += encoded.value();
    const std::uint32_t payload_crc = internal::crc32(std::string_view(encoded.value()));
    for (std::size_t index = 0; index < internal::kTrailerBytes; ++index) {
        image.push_back(static_cast<char>((payload_crc >> ((3U - index) * 8U)) & 0xFFU));
    }
    const Outcome<std::filesystem::path> staged =
        internal::stage_file(impl.tmp, generation_file_name(next), image);
    if (!staged) {
        return staged.error();
    }
    // Verify the staged bytes by re-reading them. Publication only happens after
    // the bytes on disk have been shown to be exactly the bytes intended.
    const Outcome<std::string> readback =
        internal::read_file_bounded(staged.value(), static_cast<std::uint64_t>(image.size()) + 4096U);
    if (!readback) {
        (void)internal::remove_file(staged.value());
        return readback.error();
    }
    if (readback.value().size() != image.size()) {
        (void)internal::remove_file(staged.value());
        return make_error(ErrorCode::StoreIntegrityFailed,
                          "staged generation changed size between writing and re-reading");
    }
    {
        const auto readback_header = internal::decode_generation_header(readback.value());
        if (!readback_header) {
            (void)internal::remove_file(staged.value());
            return readback_header.error();
        }
        const std::string_view payload_view =
            std::string_view(readback.value())
                .substr(internal::kHeaderBytes, static_cast<std::size_t>(readback_header.value().payload_bytes));
        if (internal::crc32(payload_view) != payload_crc) {
            (void)internal::remove_file(staged.value());
            return make_error(ErrorCode::StoreIntegrityFailed,
                              "staged generation payload checksum changed between writing and re-reading");
        }
    }
    const std::filesystem::path destination = impl.generations / generation_file_name(next);
    const Outcome<void> published = internal::publish_replace(staged.value(), destination);
    if (!published) {
        (void)internal::remove_file(staged.value());
        return published.error();
    }
    // Publish the pointer. Until this write is durable the previous generation
    // remains authoritative, so a failure here leaves the store unchanged.
    const Outcome<void> pointed =
        internal::write_file_flushed(impl.root / std::string(kCurrentFileName), encode_current(next));
    if (!pointed) {
        return pointed.error();
    }
    impl.meta.sequence = next.value();
    impl.meta.last_writer = writer.text();
    impl.meta.policy_fingerprint = payload.policy.to_canonical_string();
    const Outcome<std::vector<std::string>> names = internal::list_files(impl.generations);
    if (names) {
        std::vector<TransactionSequence> sequences;
        for (const std::string& name : names.value()) {
            const auto parsed = parse_generation_file_name(name);
            if (parsed.has_value()) {
                sequences.push_back(*parsed);
            }
        }
        std::sort(sequences.begin(), sequences.end());
        const std::size_t keep = std::max<std::size_t>(2, impl.stored_limits.max_retained_generations);
        if (sequences.size() > keep) {
            const std::size_t remove_count = sequences.size() - keep;
            for (std::size_t index = 0; index < remove_count; ++index) {
                const TransactionSequence victim = sequences[index];
                if (!(victim < next)) {
                    break;
                }
                const Outcome<void> removed = internal::remove_file(impl.generations / generation_file_name(victim));
                if (removed) {
                    impl.meta.chain_floor = std::max(impl.meta.chain_floor, victim.value());
                }
            }
        }
        impl.meta.retained_generations = static_cast<std::uint64_t>(std::min<std::size_t>(sequences.size(), keep));
    }
    const Outcome<void> meta_written =
        internal::write_file_flushed(impl.root / std::string(kMetaFileName), encode_meta(impl.meta));
    if (!meta_written) {
        // The generation is published and CURRENT names it, so the store remains
        // readable and authoritative. Report the metadata lag rather than
        // pretending the commit failed.
        return make_error(ErrorCode::StorePublishFailed,
                          "generation published but store metadata could not be updated: " +
                              meta_written.error().message());
    }
    const Outcome<void> synced = internal::sync_directory(impl.root);
    if (!synced) {
        return synced.error();
    }
    impl.authoritative_sequence = next;
    impl.authoritative_crc = payload_crc;
    return next;
}
const StoreLoadedState& StoreAccess::loaded_state(const Store& store) {
    return store.impl_->loaded;
}
TransactionSequence StoreAccess::current_sequence(const Store& store) {
    std::lock_guard<std::mutex> guard(store.impl_->mutex);
    return TransactionSequence(store.impl_->meta.sequence);
}
RegistryEpoch StoreAccess::current_epoch(const Store& store) {
    std::lock_guard<std::mutex> guard(store.impl_->mutex);
    return RegistryEpoch(store.impl_->meta.epoch);
}

bool StoreAccess::token_is_live(const Store& store, const AuthorityToken& token) {
    return store.is_token_live(token);
}
const RegistryLimits& StoreAccess::stored_limits(const Store& store) {
    return store.limits();
}
}  // namespace asset_registry

namespace asset_registry {

std::string_view to_string(StoreOpenMode mode) noexcept {
    switch (mode) {
        case StoreOpenMode::ReadWrite:
            return "read_write";
        case StoreOpenMode::ReadOnly:
            return "read_only";
        case StoreOpenMode::CreateIfMissing:
            return "create_if_missing";
    }
    return "read_write";
}

std::string_view to_string(RecoveryAction action) noexcept {
    switch (action) {
        case RecoveryAction::None:
            return "none";
        case RecoveryAction::RolledBackToLastValid:
            return "rolled_back_to_last_valid";
        case RecoveryAction::DamagedGenerationQuarantined:
            return "damaged_generation_quarantined";
        case RecoveryAction::RemovedOrphanTemporaries:
            return "removed_orphan_temporaries";
        case RecoveryAction::Initialised:
            return "initialised";
    }
    return "none";
}
}  // namespace asset_registry