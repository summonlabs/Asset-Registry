// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/error.hpp"

#include <array>
#include <utility>

namespace asset_registry {
namespace {

struct CodeName {
    ErrorCode code;
    std::string_view name;
};

// Stable wire names. Renaming an entry changes the persisted and CLI-visible
// contract, so names are additive only.
constexpr std::array<CodeName, 66> kCodeNames = {{
    {ErrorCode::None, "none"},
    {ErrorCode::InvalidInput, "invalid_input"},
    {ErrorCode::MalformedAssetId, "malformed_asset_id"},
    {ErrorCode::MalformedOwnerId, "malformed_owner_id"},
    {ErrorCode::MalformedWriterId, "malformed_writer_id"},
    {ErrorCode::MalformedCapabilityReference, "malformed_capability_reference"},
    {ErrorCode::MalformedLocationReference, "malformed_location_reference"},
    {ErrorCode::MalformedRackReference, "malformed_rack_reference"},
    {ErrorCode::MalformedSlotPosition, "malformed_slot_position"},
    {ErrorCode::MalformedSerialIdentity, "malformed_serial_identity"},
    {ErrorCode::MalformedSerialNumber, "malformed_serial_number"},
    {ErrorCode::MalformedManufacturerName, "malformed_manufacturer_name"},
    {ErrorCode::MalformedModelName, "malformed_model_name"},
    {ErrorCode::MalformedText, "malformed_text"},
    {ErrorCode::MalformedLabelKey, "malformed_label_key"},
    {ErrorCode::EmptyRequiredField, "empty_required_field"},
    {ErrorCode::TextTooLong, "text_too_long"},
    {ErrorCode::UnknownAssetClass, "unknown_asset_class"},
    {ErrorCode::InvalidConfiguration, "invalid_configuration"},
    {ErrorCode::InvalidLimits, "invalid_limits"},
    {ErrorCode::DuplicateAssetId, "duplicate_asset_id"},
    {ErrorCode::DuplicateSerialIdentity, "duplicate_serial_identity"},
    {ErrorCode::AliasConflict, "alias_conflict"},
    {ErrorCode::AssetNotFound, "asset_not_found"},
    {ErrorCode::IdentityReuseForbidden, "identity_reuse_forbidden"},
    {ErrorCode::StaleRevision, "stale_revision"},
    {ErrorCode::StaleGeneration, "stale_generation"},
    {ErrorCode::StaleAuthorityEpoch, "stale_authority_epoch"},
    {ErrorCode::AuthorityRevoked, "authority_revoked"},
    {ErrorCode::AuthorityExpired, "authority_expired"},
    {ErrorCode::StaleMutationSequence, "stale_mutation_sequence"},
    {ErrorCode::IdempotencyConflict, "idempotency_conflict"},
    {ErrorCode::IllegalLifecycleTransition, "illegal_lifecycle_transition"},
    {ErrorCode::IllegalInstallationTransition, "illegal_installation_transition"},
    {ErrorCode::LifecycleInvariantViolation, "lifecycle_invariant_violation"},
    {ErrorCode::ReplacementCycleDetected, "replacement_cycle_detected"},
    {ErrorCode::LineageTraversalLimitExceeded, "lineage_traversal_limit_exceeded"},
    {ErrorCode::SelfReplacementForbidden, "self_replacement_forbidden"},
    {ErrorCode::ReferenceAlreadyAttached, "reference_already_attached"},
    {ErrorCode::ReferenceNotAttached, "reference_not_attached"},
    {ErrorCode::ReferenceKindMismatch, "reference_kind_mismatch"},
    {ErrorCode::CapacityExceeded, "capacity_exceeded"},
    {ErrorCode::ReferenceCountExceeded, "reference_count_exceeded"},
    {ErrorCode::ProvenanceLimitExceeded, "provenance_limit_exceeded"},
    {ErrorCode::AllocationFailed, "allocation_failed"},
    {ErrorCode::TransactionNotActive, "transaction_not_active"},
    {ErrorCode::TransactionAlreadyActive, "transaction_already_active"},
    {ErrorCode::TransactionAborted, "transaction_aborted"},
    {ErrorCode::StoreOpenFailed, "store_open_failed"},
    {ErrorCode::StoreCorrupt, "store_corrupt"},
    {ErrorCode::StoreVersionUnsupported, "store_version_unsupported"},
    {ErrorCode::StoreIntegrityFailed, "store_integrity_failed"},
    {ErrorCode::StoreLocked, "store_locked"},
    {ErrorCode::StoreIoError, "store_io_error"},
    {ErrorCode::StoreNotFound, "store_not_found"},
    {ErrorCode::StorePublishFailed, "store_publish_failed"},
    {ErrorCode::StoreLayoutInvalid, "store_layout_invalid"},
    {ErrorCode::StoreRecoveryImpossible, "store_recovery_impossible"},
    {ErrorCode::ImportFormatUnsupported, "import_format_unsupported"},
    {ErrorCode::ImportTruncated, "import_truncated"},
    {ErrorCode::ImportRecordInvalid, "import_record_invalid"},
    {ErrorCode::ExportFormatUnsupported, "export_format_unsupported"},
    {ErrorCode::RegistryClosed, "registry_closed"},
    {ErrorCode::RegistryAlreadyOpen, "registry_already_open"},
    {ErrorCode::OperationAborted, "operation_aborted"},
    {ErrorCode::InternalInvariantViolation, "internal_invariant_violation"},
}};

}  // namespace

std::string_view error_code_name(ErrorCode code) noexcept {
    for (const CodeName& entry : kCodeNames) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    return "unknown_error_code";
}

std::optional<ErrorCode> error_code_from_name(std::string_view name) noexcept {
    for (const CodeName& entry : kCodeNames) {
        if (entry.name == name) {
            return entry.code;
        }
    }
    return std::nullopt;
}

bool is_staleness_error(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::StaleRevision:
        case ErrorCode::StaleGeneration:
        case ErrorCode::StaleAuthorityEpoch:
        case ErrorCode::StaleMutationSequence:
        case ErrorCode::AuthorityExpired:
            return true;
        default:
            return false;
    }
}

bool is_input_error(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::InvalidInput:
        case ErrorCode::MalformedAssetId:
        case ErrorCode::MalformedOwnerId:
        case ErrorCode::MalformedWriterId:
        case ErrorCode::MalformedCapabilityReference:
        case ErrorCode::MalformedLocationReference:
        case ErrorCode::MalformedRackReference:
        case ErrorCode::MalformedSlotPosition:
        case ErrorCode::MalformedSerialIdentity:
        case ErrorCode::MalformedSerialNumber:
        case ErrorCode::MalformedManufacturerName:
        case ErrorCode::MalformedModelName:
        case ErrorCode::MalformedText:
        case ErrorCode::MalformedLabelKey:
        case ErrorCode::EmptyRequiredField:
        case ErrorCode::TextTooLong:
        case ErrorCode::UnknownAssetClass:
        case ErrorCode::ImportFormatUnsupported:
        case ErrorCode::ImportTruncated:
        case ErrorCode::ImportRecordInvalid:
            return true;
        default:
            return false;
    }
}

bool is_storage_error(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::StoreOpenFailed:
        case ErrorCode::StoreCorrupt:
        case ErrorCode::StoreVersionUnsupported:
        case ErrorCode::StoreIntegrityFailed:
        case ErrorCode::StoreLocked:
        case ErrorCode::StoreIoError:
        case ErrorCode::StoreNotFound:
        case ErrorCode::StorePublishFailed:
        case ErrorCode::StoreLayoutInvalid:
        case ErrorCode::StoreRecoveryImpossible:
            return true;
        default:
            return false;
    }
}

Error::Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

std::string Error::to_string() const {
    std::string result(error_code_name(code_));
    result += ": ";
    result += message_;
    if (!subject_.empty()) {
        result += " [subject=";
        result += subject_;
        result += ']';
    }
    if (!detail_key_.empty()) {
        result += " [";
        result += detail_key_;
        result += '=';
        result += detail_value_;
        result += ']';
    }
    return result;
}

Error& Error::with_subject(std::string subject) {
    subject_ = std::move(subject);
    return *this;
}

Error& Error::with_detail(std::string key, std::string value) {
    detail_key_ = std::move(key);
    detail_value_ = std::move(value);
    return *this;
}

Error make_error(ErrorCode code, std::string_view message) {
    return Error(code, std::string(message));
}

}  // namespace asset_registry
