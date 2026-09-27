// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Wire-format structure test. A durable payload is a fixed sequence of members, so
// this asserts the sequence a traversal actually sees. An encoder and decoder that
// disagree about layout would otherwise surface as an unrelated parse failure deep
// inside the record codec.

#include <string>
#include <vector>

#include "test_harness.hpp"
#include "test_support.hpp"

#include "asset_registry/asset_registry.hpp"
#include "store_format.hpp"
#include "tlv.hpp"

using namespace asset_registry;
using namespace asset_registry::internal;
using namespace asset_test;

namespace {

struct Member {
    std::uint64_t tag = 0;
    std::uint64_t payload = 0;
    std::uint64_t declared = 0;
};

/// Walks the root member sequence, draining each list member so the traversal stays
/// in step, and reports what it saw.
[[nodiscard]] std::vector<Member> root_members(std::string_view buffer) {
    std::vector<Member> members;
    TlvReader reader(buffer, 64);
    reader.begin();
    if (!reader.next() || !reader.expect(0x0101ULL)) {
        return members;
    }
    std::uint64_t count = 0;
    if (!reader.read_list(64, count)) {
        return members;
    }
    for (std::uint64_t index = 0; index < count; ++index) {
        if (!reader.next()) {
            break;
        }
        Member member;
        member.tag = reader.tag();
        member.payload = reader.payload_size();
        TlvReader probe = reader;
        std::uint64_t inner = 0;
        if (probe.read_list(8192, inner)) {
            member.declared = inner;
            const bool entered = reader.read_list(8192, inner);
            AR_CHECK(entered);
            reader.leave_list();
        } else {
            reader.skip();
        }
        members.push_back(member);
    }
    return members;
}

}  // namespace

AR_TEST(wire, durable_payload_member_sequence_is_fixed) {
    StorePayload payload;
    payload.sequence = TransactionSequence(1);
    payload.epoch = RegistryEpoch(1);
    payload.limits = default_limits();

    auto encoded = encode_store_payload(payload, default_limits().max_document_bytes);
    AR_REQUIRE_OK(buffer, encoded);


    const std::vector<Member> members = root_members(buffer);
    // sequence, epoch, last writer, predecessor sequence, predecessor checksum,
    // policy, limits, records, writers.
    const std::uint64_t expected[] = {258, 259, 260, 261, 262, 272, 288, 512, 768};
    AR_CHECK_MSG(members.size() == 9, "root carries " + std::to_string(members.size()) + " members");
    if (members.size() != 9) {
        for (const Member& member : members) {
            asset_test::log_line("  member tag " + std::to_string(member.tag) + " payload " +
                                 std::to_string(member.payload));
        }
        return;
    }
    for (std::size_t index = 0; index < 9; ++index) {
        AR_CHECK_MSG(members[index].tag == expected[index],
                     "member " + std::to_string(index) + " is tag " + std::to_string(members[index].tag) +
                         " but " + std::to_string(expected[index]) + " was expected");
    }
    // The policy and bounds members carry a fixed number of elements, and the two
    // collection members carry their counts even when the inventory is empty.
    AR_CHECK(members[5].declared == 6);
    AR_CHECK(members[6].declared == 16);
    AR_CHECK_MSG(members[7].declared == 0, "records declares " + std::to_string(members[7].declared));
    AR_CHECK_MSG(members[8].declared == 0, "writers declares " + std::to_string(members[8].declared));

    // The same payload decodes, which is the property the structure above protects.
    DecodeLimits decode_limits;
    decode_limits.limits = default_limits();
    decode_limits.max_bytes = default_limits().max_document_bytes;
    decode_limits.max_depth = default_limits().max_traversal_depth;
    auto decoded = decode_store_payload(buffer, decode_limits);
    AR_REQUIRE_OK(result, decoded);
    AR_CHECK(result.records.empty());
    AR_CHECK(result.writers.empty());
    AR_CHECK(result.epoch.value() == 1);
}
