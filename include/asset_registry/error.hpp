// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Outcome<T>: the explicit success-or-failure return type used by every
// fallible Asset Registry operation. There is no exception-based control flow
// across the public API boundary and no sentinel "invalid" values.

#ifndef ASSET_REGISTRY_ERROR_HPP
#define ASSET_REGISTRY_ERROR_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(_WIN32) && defined(ASSET_REGISTRY_SHARED)
#if defined(ASSET_REGISTRY_BUILDING_LIBRARY)
#define ASSET_REGISTRY_API __declspec(dllexport)
#else
#define ASSET_REGISTRY_API __declspec(dllimport)
#endif
#else
#define ASSET_REGISTRY_API
#endif

namespace asset_registry {

/// Machine-readable rejection category. The numeric values are part of the
/// stable public contract: they are persisted in durable state and emitted by
/// the CLI, so they must never be renumbered. New categories are appended.
enum class ErrorCode : std::uint16_t {
    None = 0,

    // --- Input syntax and domain validation -------------------------------
    InvalidInput = 1,
    MalformedAssetId = 2,
    MalformedOwnerId = 3,
    MalformedWriterId = 4,
    MalformedCapabilityReference = 5,
    MalformedLocationReference = 6,
    MalformedRackReference = 7,
    MalformedSlotPosition = 8,
    MalformedSerialIdentity = 9,
    MalformedSerialNumber = 10,
    MalformedManufacturerName = 11,
    MalformedModelName = 12,
    MalformedText = 13,
    MalformedLabelKey = 14,
    EmptyRequiredField = 15,
    TextTooLong = 16,
    UnknownAssetClass = 17,

    // --- Configuration and construction -----------------------------------
    InvalidConfiguration = 18,
    InvalidLimits = 19,

    // --- Identity and uniqueness ------------------------------------------
    DuplicateAssetId = 20,
    DuplicateSerialIdentity = 21,
    AliasConflict = 22,
    AssetNotFound = 23,
    IdentityReuseForbidden = 24,

    // --- Concurrency, authority, staleness --------------------------------
    StaleRevision = 25,
    StaleGeneration = 26,
    StaleAuthorityEpoch = 27,
    AuthorityRevoked = 28,
    AuthorityExpired = 29,
    StaleMutationSequence = 30,
    IdempotencyConflict = 31,

    // --- State machines ---------------------------------------------------
    IllegalLifecycleTransition = 32,
    IllegalInstallationTransition = 33,
    LifecycleInvariantViolation = 34,

    // --- Replacement lineage ----------------------------------------------
    ReplacementCycleDetected = 35,
    LineageTraversalLimitExceeded = 36,
    SelfReplacementForbidden = 37,

    // --- Referential integrity --------------------------------------------
    ReferenceAlreadyAttached = 38,
    ReferenceNotAttached = 39,
    ReferenceKindMismatch = 40,

    // --- Resource bounds --------------------------------------------------
    CapacityExceeded = 41,
    ReferenceCountExceeded = 42,
    ProvenanceLimitExceeded = 43,
    AllocationFailed = 44,

    // --- Transactions -----------------------------------------------------
    TransactionNotActive = 45,
    TransactionAlreadyActive = 46,
    TransactionAborted = 47,

    // --- Persistence ------------------------------------------------------
    StoreOpenFailed = 48,
    StoreCorrupt = 49,
    StoreVersionUnsupported = 50,
    StoreIntegrityFailed = 51,
    StoreLocked = 52,
    StoreIoError = 53,
    StoreNotFound = 54,
    StorePublishFailed = 55,
    StoreLayoutInvalid = 56,
    StoreRecoveryImpossible = 57,

    // --- Import / export --------------------------------------------------
    ImportFormatUnsupported = 58,
    ImportTruncated = 59,
    ImportRecordInvalid = 60,
    ExportFormatUnsupported = 61,

    // --- Process lifecycle ------------------------------------------------
    RegistryClosed = 62,
    RegistryAlreadyOpen = 63,
    OperationAborted = 64,

    // --- Internal ---------------------------------------------------------
    InternalInvariantViolation = 65,
};

/// Stable machine-readable name of an error code (lower snake case).
[[nodiscard]] ASSET_REGISTRY_API std::string_view error_code_name(ErrorCode code) noexcept;

/// Parses the stable name produced by error_code_name. Returns std::nullopt for
/// unknown names; callers must not treat an unknown name as a valid code.
[[nodiscard]] ASSET_REGISTRY_API std::optional<ErrorCode> error_code_from_name(std::string_view name) noexcept;

/// True when the category indicates the caller's view of the record was out of
/// date. Such rejections are always safe to retry after re-reading.
[[nodiscard]] ASSET_REGISTRY_API bool is_staleness_error(ErrorCode code) noexcept;

/// True when the category indicates untrusted input failed validation.
[[nodiscard]] ASSET_REGISTRY_API bool is_input_error(ErrorCode code) noexcept;

/// True when the category indicates durable storage could not be trusted.
[[nodiscard]] ASSET_REGISTRY_API bool is_storage_error(ErrorCode code) noexcept;

/// A machine-readable rejection with a concise human explanation and optional
/// structured context. Errors are values: they are cheap to copy and never carry
/// ownership of registry state.
class ASSET_REGISTRY_API Error {
public:
    Error() = default;

    Error(ErrorCode code, std::string message);

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::None; }

    /// Concise human-readable explanation of the failed precondition.
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// Optional identity of the record the rejection concerns.
    [[nodiscard]] const std::string& subject() const noexcept { return subject_; }

    /// Optional machine-readable structured detail ("observed_revision" and so on).
    [[nodiscard]] const std::string& detail_key() const noexcept { return detail_key_; }
    [[nodiscard]] const std::string& detail_value() const noexcept { return detail_value_; }

    [[nodiscard]] bool is_staleness() const noexcept { return is_staleness_error(code_); }
    [[nodiscard]] bool is_input() const noexcept { return is_input_error(code_); }
    [[nodiscard]] bool is_storage() const noexcept { return is_storage_error(code_); }

    /// Single-line rendering: "code: message".
    [[nodiscard]] std::string to_string() const;

    Error& with_subject(std::string subject);
    Error& with_detail(std::string key, std::string value);

    friend bool operator==(const Error& lhs, const Error& rhs) noexcept {
        return lhs.code_ == rhs.code_ && lhs.message_ == rhs.message_ && lhs.subject_ == rhs.subject_ &&
               lhs.detail_key_ == rhs.detail_key_ && lhs.detail_value_ == rhs.detail_value_;
    }
    friend bool operator!=(const Error& lhs, const Error& rhs) noexcept { return !(lhs == rhs); }

private:
    ErrorCode code_ = ErrorCode::None;
    std::string message_;
    std::string subject_;
    std::string detail_key_;
    std::string detail_value_;
};

/// Constructs an Error from a message that outlives the call.
[[nodiscard]] ASSET_REGISTRY_API Error make_error(ErrorCode code, std::string_view message);

/// Success-or-failure carrier for every fallible operation.
template <typename T>
class Outcome {
public:
    Outcome(T value) : value_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
    Outcome(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return value_.value(); }
    [[nodiscard]] const T& value() const& { return value_.value(); }
    [[nodiscard]] T&& value() && { return std::move(value_.value()); }

    [[nodiscard]] const Error& error() const noexcept { return error_; }
    [[nodiscard]] ErrorCode code() const noexcept { return error_.code(); }

    /// Value if present, otherwise the supplied fallback.
    template <typename U>
    [[nodiscard]] T value_or(U&& fallback) const& {
        return value_.has_value() ? *value_ : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::optional<T> value_;
    Error error_;
};

/// Outcome specialisation for operations with no produced value.
template <>
class Outcome<void> {
public:
    Outcome() = default;
    Outcome(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return error_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] const Error& error() const noexcept { return error_; }
    [[nodiscard]] ErrorCode code() const noexcept { return error_.code(); }

private:
    Error error_;
};

using Status = Outcome<void>;

static_assert(std::is_copy_constructible_v<Error>, "Error must be copyable");
static_assert(std::is_copy_constructible_v<Status>, "Status must be copyable");

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_ERROR_HPP
