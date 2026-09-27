// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Write authority.
//
// Mutation authority over a store is explicit, revocable, and epoch-fenced:
//
//   * A Store mints an AuthorityToken for a writer. The token names the writer,
//     a per-epoch grant generation, and the registry epoch in force when it was
//     minted.
//   * Every mutation must present a live token. A token minted by an earlier
//     epoch is rejected with StaleAuthorityEpoch, so a process that lost and
//     regained the store lease cannot keep mutating with pre-restart authority.
//   * Tokens are revoked on release, on explicit revoke, and when the store
//     closes. A revoked token can never publish, including for a mutation that
//     had already been validated.
//
// Mutations are also sequence-checked. Each mutation presents a MutationSequence
// that must advance for the writer; a repeated sequence with a matching
// idempotency key replays the recorded outcome, and a repeated sequence without
// one is rejected as a retry the registry cannot prove safe.

#ifndef ASSET_REGISTRY_AUTHORITY_HPP
#define ASSET_REGISTRY_AUTHORITY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "asset_registry/error.hpp"
#include "asset_registry/provenance.hpp"
#include "asset_registry/reference.hpp"
#include "asset_registry/strong_types.hpp"

namespace asset_registry {

/// Options applied when minting a token.
struct ASSET_REGISTRY_API AuthorityOptions {
    /// Lifetime of the grant. Absent means the grant lives until it is released,
    /// revoked, or the store closes. When a lifetime is supplied, an expired
    /// token is rejected with AuthorityExpired. Expiry is evaluated against the
    /// registry clock, so an injected test clock makes expiry deterministic.
    std::optional<std::int64_t> ttl_nanos;

    /// Operator-visible explanation recorded with the grant for diagnostics.
    std::string reason;
};

/// An unforgeable-by-value grant of mutation authority.
///
/// The token carries no pointers: it is validated against live registry state on
/// every use, so copying it confers no additional authority and a token that has
/// been revoked stays revoked in every copy.
class ASSET_REGISTRY_API AuthorityToken {
public:
    AuthorityToken() = default;

    [[nodiscard]] const WriterId& writer() const noexcept { return writer_; }
    [[nodiscard]] AuthorityGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] RegistryEpoch epoch() const noexcept { return epoch_; }
    [[nodiscard]] const std::optional<Timestamp>& expires_at() const noexcept { return expires_at_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

    /// True when the value carries a writer and a grant generation. It says
    /// nothing about whether the grant is still live; only the registry can
    /// decide that.
    [[nodiscard]] bool is_present() const noexcept { return !writer_.empty() && !generation_.is_zero(); }

    [[nodiscard]] std::string to_string() const;

private:
    friend class AssetRegistry;
    friend class Store;

    WriterId writer_;
    AuthorityGeneration generation_;
    RegistryEpoch epoch_;
    std::optional<Timestamp> expires_at_;
    std::string reason_;
};

/// Whether a mutation is allowed to proceed without a live registry token.
///
/// LocalAuthority exists so that detached, in-memory registries (used for
/// validation, unit testing, and canonical export assembly) can be mutated
/// without pretending a durable store exists. A registry backed by a durable
/// store always requires RegistryAuthority: there is no configuration switch to
/// relax that, because relaxing it would let a consumer bypass epoch fencing.
enum class AuthorityMode : std::uint8_t {
    /// The caller must present a token minted by the registry that owns the store.
    RegistryAuthority = 0,
    /// The registry owns no durable store, so mutations are validated against
    /// in-memory state only. Reported as such by authority_mode().
    LocalAuthority = 1,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(AuthorityMode value) noexcept;

/// A mutation envelope: the writer's sequence position and optional retry key.
///
/// Supplying an idempotency key makes a mutation retry-safe. Re-issuing the same
/// key with the same sequence and an identical request replays the recorded
/// result without applying the change twice. Re-using a key with a different
/// request is rejected with IdempotencyConflict rather than applied.
struct ASSET_REGISTRY_API MutationEnvelope {
    MutationSequence sequence;

    /// Optional retry key. When absent, the mutation is applied exactly once and
    /// any later attempt reusing the same sequence is rejected with
    /// StaleMutationSequence.
    std::optional<std::string> idempotency_key;

    /// Optional diagnostic reason recorded in provenance for this mutation.
    std::string reason;
};

/// A writer's handle on a registry: the authority token plus the monotonically
/// advancing mutation sequence this writer has reached.
///
/// A session is a value with local state, and it is deliberately not thread
/// safe. One session belongs to one logical writer; sharing a session across
/// threads requires external synchronisation, which is the caller's decision
/// rather than an implicit guarantee.
class ASSET_REGISTRY_API WriterSession {
public:
    WriterSession() = default;

    /// A session positioned at its first mutation. The authority token is filled
    /// in by Store::open_writer; a session that never received a token is
    /// rejected by every mutation with AuthorityRevoked.
    [[nodiscard]] static WriterSession create(WriterId writer);

    /// Continues a writer's sequence after a restart, so a reissued request is
    /// recognised as a retry rather than applied again.
    [[nodiscard]] static WriterSession resume(WriterId writer, MutationSequence next_sequence);

    [[nodiscard]] const WriterId& writer() const noexcept { return writer_; }
    [[nodiscard]] const AuthorityToken& token() const noexcept { return token_; }

    /// Next sequence this session will present. Starts at 1.
    [[nodiscard]] MutationSequence next_sequence() const noexcept { return next_sequence_; }

    /// Advances the sequence and returns the envelope to pass to a mutation.
    /// `idempotency_key` is optional; when present, the mutation becomes
    /// retry-safe. `reason` is recorded in provenance.
    [[nodiscard]] MutationEnvelope advance(std::optional<std::string> idempotency_key = std::nullopt,
                                           std::string reason = {});

    /// Re-arms the session at a specific sequence. Used after a failed mutation
    /// whose effects were not published, so the writer does not burn sequence
    /// numbers on rejected requests. Presenting a sequence at or below the
    /// registry's recorded high-water mark still requires a matching
    /// idempotency record, so rewinding cannot be used to replay a mutation.
    void rewind_to(MutationSequence sequence) noexcept { next_sequence_ = sequence; }

    /// Attaches the authority token minted for this writer by a store. Called by
    /// the store; calling it directly has no effect on whether the registry
    /// accepts the token, which is validated against live registry state.
    void attach_token(AuthorityToken token) noexcept { token_ = std::move(token); }

    /// True when a token has been attached.
    [[nodiscard]] bool is_armed() const noexcept { return token_.is_present(); }

private:
    WriterId writer_;
    AuthorityToken token_;
    MutationSequence next_sequence_{1};
};

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_AUTHORITY_HPP
