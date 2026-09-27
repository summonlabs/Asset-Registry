// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Level-one codec tests for the framed tag/length/value wire format and the durable
// payload codec. These are asserted directly because every durable behaviour depends on
// them, and a fault here would surface as an unrelated failure further up.

#include <cstdio>
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

constexpr std::uint64_t kTagScalar = 0x0001;
constexpr std::uint64_t kTagText = 0x0002;
constexpr std::uint64_t kTagList = 0x0003;
constexpr std::uint64_t kTagChild = 0x0004;
constexpr std::uint64_t kTagEmpty = 0x0005;

[[nodiscard]] std::string hex_of(std::string_view bytes) {
    std::string text;
    for (const char raw : bytes) {
        char octet[4];
        std::snprintf(octet, sizeof(octet), "%02x", static_cast<unsigned char>(raw));
        text += octet;
    }
    return text;
}

/// Encodes one scalar, one string, and one list of two nested elements. The byte form is
/// pinned by `sample_bytes_are_exact`, so the reader tests work against an encoder that is
/// independently verified.
[[nodiscard]] std::string encode_sample() {
    TlvWriter writer;
    writer.append_unsigned(kTagScalar, 300);
    writer.append_string(kTagText, "hello");
    writer.open_list(kTagList, 2);
    writer.append_unsigned(kTagChild, 1);
    writer.append_unsigned(kTagChild, 2);
    writer.close_list();
    AR_CHECK(writer.balanced());
    return writer.take();
}

}  // namespace

AR_TEST(codec, writer_output_is_exact_for_each_shape) {
    // One scalar.
    {
        TlvWriter writer;
        writer.append_unsigned(kTagScalar, 1);
        AR_CHECK_MSG(hex_of(writer.take()) == "010101", "scalar");
    }
    // One list with one scalar child.
    {
        TlvWriter writer;
        writer.open_list(kTagList, 1);
        writer.append_unsigned(kTagChild, 1);
        writer.close_list();
        AR_CHECK_MSG(hex_of(writer.take()) == "030401040101", "one child");
    }
    // One list with two scalar children.
    {
        TlvWriter writer;
        writer.open_list(kTagList, 2);
        writer.append_unsigned(kTagChild, 1);
        writer.append_unsigned(kTagChild, 2);
        writer.close_list();
        AR_CHECK_MSG(hex_of(writer.take()) == "030702040101040102", "two children");
    }
    // An empty list.
    {
        TlvWriter writer;
        writer.open_list(kTagList, 0);
        writer.close_list();
        AR_CHECK_MSG(hex_of(writer.take()) == "030100", "empty list");
    }
    // A list holding a nested list.
    {
        TlvWriter writer;
        writer.open_list(kTagList, 1);
        writer.open_list(kTagChild, 1);
        writer.append_unsigned(kTagScalar, 7);
        writer.close_list();
        writer.close_list();
        const std::string nested = writer.take();
        AR_CHECK_MSG(hex_of(nested) == "03070104040101" "0107", hex_of(nested));
    }
}

AR_TEST(codec, sample_bytes_are_exact) {
    // The wire form of the sample, byte for byte. This pins the encoder independently of
    // the reader, so a disagreement between the two cannot be mistaken for a traversal
    // fault. Integers use the smallest width that holds the value.
    const std::string expected =
        "0102" "012c"                  // tag 1, 2-byte payload, 300
        "0205" "68656c6c6f"            // tag 2, 5-byte payload, "hello"
        "0307" "02"                    // tag 3, 7-byte payload: count plus two children
        "0401" "01"                    // tag 4, 1-byte payload, 1
        "0401" "02";                   // tag 4, 1-byte payload, 2
    const std::string buffer = encode_sample();
    AR_CHECK_MSG(hex_of(buffer) == expected, "encoded " + hex_of(buffer) + " but expected " + expected);
}

AR_TEST(codec, scalars_and_strings_round_trip) {
    const std::string buffer = encode_sample();
    TlvReader reader(buffer, 16);
    reader.begin();

    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagScalar));
    std::uint64_t scalar = 0;
    AR_CHECK(reader.read_unsigned(scalar));
    AR_CHECK(scalar == 300);

    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagText));
    std::string text;
    AR_CHECK(reader.read_string(text));
    AR_CHECK(text == "hello");

    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagList));
    AR_CHECK(reader.payload_size() == 7);
    std::uint64_t count = 0;
    AR_REQUIRE_CHECK(reader.read_list(4, count));
    AR_CHECK(count == 2);

    std::vector<std::uint64_t> children;
    for (std::uint64_t index = 0; index < count; ++index) {
        AR_REQUIRE_CHECK(reader.next());
        AR_REQUIRE_CHECK(reader.expect(kTagChild));
        std::uint64_t value = 0;
        AR_CHECK(reader.read_unsigned(value));
        children.push_back(value);
    }
    AR_CHECK(children.size() == 2);
    AR_CHECK(children[0] == 1);
    AR_CHECK(children[1] == 2);

    AR_CHECK(!reader.next());
    AR_CHECK_MSG(reader.status() == TlvStatus::Ok, std::string(to_string(reader.status())));
    AR_CHECK(reader.exhausted());
}

AR_TEST(codec, reaching_the_end_of_input_is_not_an_error) {
    const std::string buffer = encode_sample();
    TlvReader reader(buffer, 16);
    reader.begin();
    while (reader.next()) {
        reader.skip();
    }
    AR_CHECK_MSG(reader.status() == TlvStatus::Ok, std::string(to_string(reader.status())));
    AR_CHECK(reader.complete());
}

AR_TEST(codec, one_list_with_one_child) {
    TlvWriter writer;
    writer.open_list(kTagList, 1);
    writer.append_unsigned(kTagChild, 42);
    writer.close_list();
    const std::string buffer = writer.take();
    AR_CHECK_MSG(hex_of(buffer) == "03040104012a", "one-child list encoded as " + hex_of(buffer));

    TlvReader reader(buffer, 16);
    reader.begin();
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagList));
    std::uint64_t count = 0;
    AR_REQUIRE_CHECK(reader.read_list(4, count));
    AR_CHECK(count == 1);
    AR_CHECK_MSG(reader.offset() == 3, "cursor after read_list is " + std::to_string(reader.offset()));
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagChild));
    std::uint64_t value = 0;
    AR_CHECK(reader.read_unsigned(value));
    AR_CHECK(value == 42);
    AR_CHECK(!reader.next());
    AR_CHECK(reader.complete());
}

AR_TEST(codec, deep_nesting_is_iterated_in_order) {
    // outer -> [ child -> [ 7 ], child -> [ list -> [ 9 ] ] ]
    TlvWriter writer;
    writer.open_list(kTagList, 2);
    writer.open_list(kTagChild, 1);
    writer.append_unsigned(kTagScalar, 7);
    writer.close_list();
    writer.open_list(kTagChild, 1);
    writer.open_list(kTagList, 1);
    writer.append_unsigned(kTagScalar, 9);
    writer.close_list();
    writer.close_list();
    writer.close_list();
    AR_CHECK(writer.balanced());
    const std::string buffer = writer.take();

    TlvReader reader(buffer, 16);
    reader.begin();
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagList));
    std::uint64_t outer = 0;
    AR_REQUIRE_CHECK(reader.read_list(4, outer));
    AR_CHECK(outer == 2);

    // First child: a list holding one scalar.
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagChild));
    std::uint64_t first = 0;
    AR_REQUIRE_CHECK(reader.read_list(2, first));
    AR_CHECK(first == 1);
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagScalar));
    std::uint64_t seven = 0;
    AR_CHECK(reader.read_unsigned(seven));
    AR_CHECK(seven == 7);

    // Advancing now leaves the finished inner list and lands on the second child.
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagChild));
    std::uint64_t second = 0;
    AR_REQUIRE_CHECK(reader.read_list(2, second));
    AR_CHECK(second == 1);
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagList));
    std::uint64_t inner = 0;
    AR_REQUIRE_CHECK(reader.read_list(2, inner));
    AR_CHECK(inner == 1);
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagScalar));
    std::uint64_t nine = 0;
    AR_CHECK(reader.read_unsigned(nine));
    AR_CHECK(nine == 9);

    AR_CHECK(!reader.next());
    AR_CHECK(reader.complete());
}

AR_TEST(codec, empty_lists_and_lists_of_lists) {
    // An empty list, then a list whose two children are both empty lists.
    TlvWriter writer;
    writer.open_list(kTagEmpty, 0);
    writer.close_list();
    writer.open_list(kTagList, 2);
    writer.open_list(kTagChild, 0);
    writer.close_list();
    writer.open_list(kTagChild, 0);
    writer.close_list();
    writer.close_list();
    writer.close_list();
    AR_CHECK(writer.balanced());
    const std::string buffer = writer.take();


    // The outer list's payload is its count plus its two three-byte children: the declared
    // payload length covers the count byte, so it is seven and not six.
    AR_CHECK_MSG(hex_of(buffer) == "050100" "0307" "02" "040100" "040100",
                 "encoded " + hex_of(buffer));

    TlvReader reader(buffer, 16);
    reader.begin();

    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagEmpty));
    std::uint64_t empty_count = 99;
    AR_REQUIRE_CHECK_MSG(reader.read_list(4, empty_count),
                         std::string(to_string(reader.status())) + " tag " + std::to_string(reader.tag()) +
                             " payload " + std::to_string(reader.payload_size()));
    AR_CHECK(empty_count == 0);

    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagList));
    std::uint64_t count = 0;
    AR_REQUIRE_CHECK(reader.read_list(4, count));
    AR_CHECK(count == 2);
    for (std::uint64_t index = 0; index < count; ++index) {
        AR_REQUIRE_CHECK(reader.next());
        AR_REQUIRE_CHECK(reader.expect(kTagChild));
        std::uint64_t inner = 99;
        AR_REQUIRE_CHECK(reader.read_list(4, inner));
        AR_CHECK(inner == 0);
    }
    AR_CHECK(!reader.next());
    AR_CHECK(reader.complete());
}

AR_TEST(codec, a_top_level_empty_list_advances_to_the_next_element) {
    // An empty list is one element like any other: reading it as a list yields no children and
    // leaves the cursor after the whole element, so the next element is the one that follows it.
    TlvWriter writer;
    writer.open_list(kTagEmpty, 0);
    writer.close_list();
    writer.append_unsigned(kTagScalar, 7);
    writer.open_list(kTagEmpty, 0);
    writer.close_list();
    const std::string buffer = writer.take();
    AR_CHECK_MSG(hex_of(buffer) == "050100" "010107" "050100", "encoded " + hex_of(buffer));

    TlvReader reader(buffer, 16);
    reader.begin();
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagEmpty));
    std::uint64_t first = 99;
    AR_REQUIRE_CHECK(reader.read_list(4, first));
    AR_CHECK(first == 0);
    AR_CHECK_MSG(reader.offset() == 3, "cursor after the empty list is " + std::to_string(reader.offset()));

    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagScalar));
    std::uint64_t seven = 0;
    AR_CHECK(reader.read_unsigned(seven));
    AR_CHECK(seven == 7);

    // An empty list as the last element also ends the traversal cleanly.
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagEmpty));
    std::uint64_t last = 99;
    AR_REQUIRE_CHECK(reader.read_list(4, last));
    AR_CHECK(last == 0);
    AR_CHECK(!reader.next());
    AR_CHECK(reader.complete());
}

AR_TEST(codec, a_child_cannot_reach_past_the_list_payload) {
    // The list declares a four-byte payload: its count byte and a three-byte child. A trailing
    // element follows the list, so a forged child has a byte to reach into that is inside the
    // buffer. Only the end of the list's own payload can stop it: the children region is the
    // payload minus the count that introduces them, not the payload end plus that count.
    TlvWriter writer;
    writer.open_list(kTagList, 1);
    writer.append_unsigned(kTagChild, 1);
    writer.close_list();
    writer.append_unsigned(kTagScalar, 7);
    std::string forged = writer.take();
    AR_CHECK_MSG(hex_of(forged) == "030401040101010107", "encoded " + hex_of(forged));
    // The child declares two payload bytes where its payload holds one, so its frame ends one byte
    // past the end of the list payload: inside the buffer, and outside the list that declares it.
    forged[4] = static_cast<char>(0x02);
    TlvReader reader(forged, 16);
    reader.begin();
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagList));
    std::uint64_t count = 0;
    AR_REQUIRE_CHECK(reader.read_list(4, count));
    AR_CHECK(count == 1);
    AR_CHECK_MSG(!reader.next(), "the overrunning child was accepted");
    AR_CHECK_MSG(reader.status() == TlvStatus::LengthOverflow, std::string(to_string(reader.status())));

    // The count itself cannot reach past the payload it introduces either: a count varint whose
    // continuation byte is the last payload byte is a malformed element, not a larger count.
    {
        std::string truncated;
        truncated.push_back(static_cast<char>(kTagList));
        truncated.push_back(static_cast<char>(0x01));
        truncated.push_back(static_cast<char>(0x80));  // continuation bit set, payload exhausted
        truncated.push_back(static_cast<char>(0x01));  // a byte of the element that follows
        TlvReader count_reader(truncated, 16);
        count_reader.begin();
        AR_REQUIRE_CHECK(count_reader.next());
        std::uint64_t declared = 0;
        AR_CHECK(!count_reader.read_list(4, declared));
        AR_CHECK_MSG(count_reader.status() == TlvStatus::LengthOverflow,
                     std::string(to_string(count_reader.status())));
    }
}

AR_TEST(codec, list_and_scalar_neighbours) {
    TlvWriter writer;
    writer.open_list(kTagList, 1);
    writer.append_unsigned(kTagChild, 5);
    writer.close_list();
    writer.append_unsigned(kTagScalar, 6);
    writer.open_list(kTagList, 0);
    writer.close_list();
    writer.append_unsigned(kTagScalar, 7);
    const std::string buffer = writer.take();

    TlvReader reader(buffer, 16);
    reader.begin();
    AR_REQUIRE_CHECK(reader.next());
    std::uint64_t count = 0;
    AR_REQUIRE_CHECK(reader.read_list(4, count));
    AR_REQUIRE_CHECK(reader.next());
    std::uint64_t five = 0;
    AR_CHECK(reader.read_unsigned(five));
    AR_CHECK(five == 5);

    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagScalar));
    std::uint64_t six = 0;
    AR_CHECK(reader.read_unsigned(six));
    AR_CHECK(six == 6);

    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagList));
    std::uint64_t empty = 99;
    AR_REQUIRE_CHECK(reader.read_list(4, empty));
    AR_CHECK(empty == 0);

    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagScalar));
    std::uint64_t seven = 0;
    AR_CHECK(reader.read_unsigned(seven));
    AR_CHECK(seven == 7);

    AR_CHECK(!reader.next());
    AR_CHECK(reader.complete());
}

AR_TEST(codec, sibling_lists_are_independent) {
    TlvWriter writer;
    writer.open_list(kTagList, 1);
    writer.append_unsigned(kTagChild, 1);
    writer.close_list();
    writer.open_list(kTagList, 2);
    writer.append_unsigned(kTagChild, 2);
    writer.append_unsigned(kTagChild, 3);
    writer.close_list();
    const std::string buffer = writer.take();

    TlvReader reader(buffer, 16);
    reader.begin();
    const std::uint64_t expected[] = {1, 2, 3};
    std::vector<std::uint64_t> seen;
    while (reader.next()) {
        AR_REQUIRE_CHECK(reader.expect(kTagList));
        std::uint64_t count = 0;
        AR_REQUIRE_CHECK(reader.read_list(4, count));
        for (std::uint64_t index = 0; index < count; ++index) {
            AR_REQUIRE_CHECK(reader.next());
            std::uint64_t value = 0;
            AR_CHECK(reader.read_unsigned(value));
            seen.push_back(value);
        }
    }
    AR_CHECK(reader.ok());
    AR_REQUIRE_CHECK(seen.size() == 3);
    for (std::size_t index = 0; index < 3; ++index) {
        AR_CHECK(seen[index] == expected[index]);
    }
}

AR_TEST(codec, skip_crosses_arbitrary_nesting) {
    const std::string buffer = encode_sample();
    TlvReader reader(buffer, 16);
    reader.begin();
    // Skip the first scalar, the string, and the whole list in three calls.
    AR_REQUIRE_CHECK(reader.next());
    reader.skip();
    AR_REQUIRE_CHECK(reader.next());
    reader.skip();
    AR_REQUIRE_CHECK(reader.next());
    reader.skip();
    AR_CHECK(!reader.next());
    AR_CHECK(reader.complete());
}

AR_TEST(codec, skip_inside_a_list_resumes_at_the_next_sibling) {
    TlvWriter writer;
    writer.open_list(kTagList, 3);
    writer.append_string(kTagChild, "skipped");
    writer.append_unsigned(kTagChild, 8);
    writer.open_list(kTagChild, 1);
    writer.append_unsigned(kTagScalar, 9);
    writer.close_list();
    writer.close_list();
    const std::string buffer = writer.take();

    TlvReader reader(buffer, 16);
    reader.begin();
    AR_REQUIRE_CHECK(reader.next());
    std::uint64_t count = 0;
    AR_REQUIRE_CHECK(reader.read_list(4, count));
    AR_CHECK(count == 3);

    AR_REQUIRE_CHECK(reader.next());
    reader.skip();
    AR_REQUIRE_CHECK(reader.next());
    std::uint64_t eight = 0;
    AR_CHECK(reader.read_unsigned(eight));
    AR_CHECK(eight == 8);
    AR_REQUIRE_CHECK(reader.next());
    // The third child is itself a list; skipping it must not disturb the outer level.
    reader.skip();
    AR_CHECK(!reader.next());
    AR_CHECK(reader.complete());
}

AR_TEST(codec, leave_list_abandons_the_remaining_children) {
    TlvWriter writer;
    writer.open_list(kTagList, 5);
    for (std::uint64_t index = 0; index < 5; ++index) {
        writer.append_unsigned(kTagChild, index);
    }
    writer.close_list();
    writer.append_unsigned(kTagScalar, 77);
    const std::string buffer = writer.take();
    AR_CHECK_MSG(hex_of(buffer) == "0310050401000401010401020401030401040101" "4d",
                 "encoded " + hex_of(buffer));

    TlvReader reader(buffer, 16);
    reader.begin();
    AR_REQUIRE_CHECK(reader.next());
    std::uint64_t count = 0;
    AR_REQUIRE_CHECK(reader.read_list(8, count));
    AR_CHECK(count == 5);
    AR_REQUIRE_CHECK(reader.next());
    std::uint64_t first = 0;
    AR_CHECK(reader.read_unsigned(first));
    AR_CHECK(first == 0);
    AR_CHECK_MSG(reader.offset() == 6, "cursor after the first child is " + std::to_string(reader.offset()));
    reader.leave_list();
    AR_CHECK_MSG(reader.offset() == 18, "cursor after leave_list is " + std::to_string(reader.offset()));
    // The abandoned list is finished, so the next element is the one after it.
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK_MSG(reader.expect(kTagScalar),
                 "saw tag " + std::to_string(reader.tag()) + " payload " + std::to_string(reader.payload_size()) +
                     " at " + std::to_string(reader.offset()));
    std::uint64_t trailing = 0;
    AR_CHECK(reader.read_unsigned(trailing));
    AR_CHECK(trailing == 77);
    AR_CHECK(!reader.next());
    AR_CHECK(reader.exhausted());
}

AR_TEST(codec, leaving_a_nested_list_returns_to_the_outer_list) {
    TlvWriter writer;
    writer.open_list(kTagList, 3);
    writer.append_unsigned(kTagChild, 1);
    writer.open_list(kTagChild, 3);
    writer.append_unsigned(kTagScalar, 10);
    writer.append_unsigned(kTagScalar, 11);
    writer.append_unsigned(kTagScalar, 12);
    writer.close_list();
    writer.append_unsigned(kTagChild, 3);
    writer.close_list();
    const std::string buffer = writer.take();

    TlvReader reader(buffer, 16);
    reader.begin();
    AR_REQUIRE_CHECK(reader.next());
    std::uint64_t outer = 0;
    AR_REQUIRE_CHECK(reader.read_list(8, outer));
    AR_CHECK(outer == 3);

    AR_REQUIRE_CHECK(reader.next());
    std::uint64_t one = 0;
    AR_CHECK(reader.read_unsigned(one));
    AR_CHECK(one == 1);

    AR_REQUIRE_CHECK(reader.next());
    std::uint64_t inner = 0;
    AR_REQUIRE_CHECK(reader.read_list(8, inner));
    AR_CHECK(inner == 3);
    AR_REQUIRE_CHECK(reader.next());
    std::uint64_t ten = 0;
    AR_CHECK(reader.read_unsigned(ten));
    AR_CHECK(ten == 10);

    // Abandoning the inner list returns the traversal to the outer list, at the element that
    // follows the inner list rather than past the end of the outer one.
    reader.leave_list();
    AR_REQUIRE_CHECK(reader.next());
    AR_CHECK(reader.expect(kTagChild));
    std::uint64_t three = 0;
    AR_CHECK(reader.read_unsigned(three));
    AR_CHECK(three == 3);
    AR_CHECK(!reader.next());
    AR_CHECK(reader.complete());
}

AR_TEST(codec, malformed_framing_is_rejected) {
    // A declared length that runs past the end of the buffer.
    {
        TlvWriter writer;
        writer.append_unsigned(kTagScalar, 1);
        std::string forged = writer.take();
        forged[1] = static_cast<char>(0x7F);
        TlvReader reader(forged, 16);
        reader.begin();
        AR_CHECK(!reader.next());
        AR_CHECK(reader.status() == TlvStatus::LengthOverflow);
    }
    // A non-minimal varint.
    {
        std::string forged;
        forged.push_back(static_cast<char>(0x81));
        forged.push_back(static_cast<char>(0x00));
        forged.push_back(static_cast<char>(0x01));
        forged.push_back(static_cast<char>(0x00));
        TlvReader reader(forged, 16);
        reader.begin();
        AR_CHECK(!reader.next());
        AR_CHECK(reader.status() == TlvStatus::NotCanonical);
    }
    // An integer encoded wider than the value needs.
    {
        std::string forged;
        forged.push_back(static_cast<char>(kTagScalar));
        forged.push_back(static_cast<char>(0x02));
        forged.push_back(static_cast<char>(0x00));
        forged.push_back(static_cast<char>(0x01));
        TlvReader reader(forged, 16);
        reader.begin();
        AR_REQUIRE_CHECK(reader.next());
        std::uint64_t value = 0;
        AR_CHECK(!reader.read_unsigned(value));
        AR_CHECK(reader.status() == TlvStatus::IntegerOverflowedWidth);
    }
    // A truncated element header.
    {
        std::string forged;
        forged.push_back(static_cast<char>(kTagScalar));
        TlvReader reader(forged, 16);
        reader.begin();
        AR_CHECK(!reader.next());
        AR_CHECK(reader.status() == TlvStatus::UnexpectedEnd);
    }
}

AR_TEST(codec, malformed_nested_lengths_are_rejected) {
    // A child whose declared length reaches past its parent's payload.
    {
        TlvWriter writer;
        writer.open_list(kTagList, 1);
        writer.append_unsigned(kTagChild, 1);
        writer.close_list();
        std::string forged = writer.take();
        // The child's own length byte is the fourth byte of the element.
        forged[4] = static_cast<char>(0x40);
        AR_CHECK_MSG(hex_of(forged) == "030401044001", "forged " + hex_of(forged));
        TlvReader reader(forged, 16);
        reader.begin();
        AR_REQUIRE_CHECK(reader.next());
        std::uint64_t count = 0;
        const bool listed = reader.read_list(4, count);
        if (!listed) {
            // A count the payload cannot hold is rejected at the list header, which is also a
            // rejection of the forged element.
            AR_CHECK_MSG(reader.status() == TlvStatus::CountExceeded || reader.status() == TlvStatus::LengthOverflow,
                         std::string(to_string(reader.status())));
        } else {
            AR_CHECK_MSG(!reader.next(), "the forged child was accepted");
            AR_CHECK_MSG(!reader.ok(), std::string(to_string(reader.status())));
        }
    }
    // A child whose declared length overruns the sibling that follows it.
    {
        TlvWriter writer;
        writer.open_list(kTagList, 2);
        writer.append_string(kTagChild, "a");
        writer.append_string(kTagChild, "b");
        writer.close_list();
        // A trailing element gives the forged child somewhere to reach into, so the fault under
        // test is the child's own overrun rather than a short buffer.
        writer.append_unsigned(kTagScalar, 3);
        std::string forged = writer.take();
        AR_CHECK_MSG(hex_of(forged).rfind("0307", 0) == 0 || hex_of(forged).rfind("0309", 0) == 0,
                     "encoded " + hex_of(forged));
        // The first child is the fourth byte of the element, and its length byte is the fifth.
        AR_CHECK(forged.size() >= 6);
        AR_CHECK(static_cast<unsigned char>(forged[3]) == 0x04);
        AR_CHECK(static_cast<unsigned char>(forged[4]) == 0x01);
        // The first child declares nine payload bytes where its payload holds one, so its frame
        // swallows the second child and reaches past the end of the list element.
        forged[4] = static_cast<char>(0x09);
        TlvReader reader(forged, 16);
        reader.begin();
        AR_REQUIRE_CHECK(reader.next());
        std::uint64_t count = 0;
        if (reader.read_list(4, count)) {
            AR_CHECK_MSG(!reader.next(),
                         "the overrunning child was accepted: tag " + std::to_string(reader.tag()) + " payload " +
                             std::to_string(reader.payload_size()) + " status " +
                             std::string(to_string(reader.status())));
        } else {
            AR_CHECK_MSG(reader.status() == TlvStatus::LengthOverflow ||
                             reader.status() == TlvStatus::CountExceeded,
                         std::string(to_string(reader.status())));
        }
    }
    // A list declaring zero children but carrying bytes.
    {
        TlvWriter writer;
        writer.open_list(kTagList, 1);
        writer.append_unsigned(kTagChild, 1);
        writer.close_list();
        std::string forged = writer.take();
        forged[2] = static_cast<char>(0x00);
        TlvReader reader(forged, 16);
        reader.begin();
        AR_REQUIRE_CHECK(reader.next());
        std::uint64_t count = 0;
        AR_CHECK(!reader.read_list(4, count));
        AR_CHECK(reader.status() == TlvStatus::LengthMismatch);
    }
    // A scalar whose declared payload is longer than the bytes that follow it in the buffer.
    {
        TlvWriter writer;
        writer.append_unsigned(kTagScalar, 1);
        writer.append_unsigned(kTagScalar, 1);
        std::string forged = writer.take();
        // The first element declares sixty-four payload bytes where the buffer holds four more,
        // so accepting it would read past the end of the input.
        forged[1] = static_cast<char>(0x40);
        TlvReader reader(forged, 16);
        reader.begin();
        AR_CHECK(!reader.next());
        AR_CHECK(reader.status() == TlvStatus::LengthOverflow);
    }

    // A last element whose declared payload runs past the end of the buffer.
    {
        TlvWriter writer;
        writer.append_unsigned(kTagScalar, 1);
        std::string forged = writer.take();
        forged[1] = static_cast<char>(0x40);
        TlvReader reader(forged, 16);
        reader.begin();
        AR_CHECK(!reader.next());
        AR_CHECK(reader.status() == TlvStatus::LengthOverflow);
    }
}

AR_TEST(codec, depth_and_count_limits_are_enforced) {
    // A bound of one permits the outermost list and nothing nested inside it.
    {
        const std::string buffer = encode_sample();
        TlvReader reader(buffer, 1);
        reader.begin();
        AR_REQUIRE_CHECK(reader.next());
        reader.skip();
        AR_REQUIRE_CHECK(reader.next());
        reader.skip();
        AR_REQUIRE_CHECK(reader.next());
        AR_CHECK(reader.expect(kTagList));
        std::uint64_t count = 0;
        AR_REQUIRE_CHECK(reader.read_list(4, count));
        AR_CHECK(count == 2);
        AR_REQUIRE_CHECK(reader.next());
        std::uint64_t nested = 0;
        AR_CHECK(!reader.read_list(4, nested));
        AR_CHECK(reader.status() == TlvStatus::DepthExceeded);
    }
    // A declared count beyond the caller's bound.
    {
        TlvWriter writer;
        writer.open_list(kTagList, 8);
        for (std::uint64_t index = 0; index < 8; ++index) {
            writer.append_unsigned(kTagChild, index);
        }
        writer.close_list();
        const std::string buffer = writer.take();
        TlvReader reader(buffer, 8);
        reader.begin();
        AR_REQUIRE_CHECK(reader.next());
        std::uint64_t count = 0;
        AR_CHECK(!reader.read_list(4, count));
        AR_CHECK(reader.status() == TlvStatus::CountExceeded);
    }
    // A scalar payload is not a list.
    {
        TlvWriter writer;
        writer.append_unsigned(kTagScalar, 1);
        const std::string buffer = writer.take();
        TlvReader reader(buffer, 8);
        reader.begin();
        AR_REQUIRE_CHECK(reader.next());
        std::uint64_t count = 0;
        AR_CHECK(!reader.read_list(4, count));
        AR_CHECK_MSG(reader.status() == TlvStatus::WrongType || reader.status() == TlvStatus::CountExceeded,
                     std::string(to_string(reader.status())));
    }
}

AR_TEST(codec, payload_round_trips_through_the_durable_codec) {
    StorePayload payload;
    payload.sequence = TransactionSequence(4);
    payload.epoch = RegistryEpoch(2);
    payload.limits = default_limits();
    payload.last_writer = "codec-writer";
    payload.predecessor_sequence = TransactionSequence(3);
    payload.predecessor_crc = 0xDEADBEEF;

    auto record = std::make_shared<AssetRecord>();
    record->id = asset_id_for("codec-asset");
    record->asset_class = AssetClass::Server;
    record->generation = AssetGeneration(1);
    record->revision = AssetRevision(1);
    record->last_sequence = TransactionSequence(4);
    record->serial_identity = serial("Codec Vendor", "CODEC-1", "R1");
    record->metadata.display_name = "codec-asset";
    record->metadata.owner = owner("org.example.platform");
    record->metadata.site = location("site-a.hall-1");
    record->metadata.notes = "notes";
    record->metadata.labels = {{"tier", "gold"}};
    record->references = {capability_ref("asi:accelerator.scheduling", ReferenceEvidence::Verified),
                          Reference::location(location("site-a.hall-1.room-1"), ReferenceEvidence::Unverified)};
    record->state.lifecycle = LifecycleState::Planned;
    record->state.installation = InstallationState::Unknown;
    ProvenanceStep step;
    step.sequence = TransactionSequence(4);
    step.epoch = RegistryEpoch(2);
    step.revision = AssetRevision(1);
    step.action = ProvenanceAction::Registered;
    step.origin = ProvenanceOrigin::ApiMutation;
    step.actor = ActorRef::writer(writer("codec-writer"));
    step.recorded_at = Timestamp(1'700'000'000'000'000'000LL);
    step.reason = "registered";
    step.change = "origin=new";
    record->provenance.push_back(step);
    payload.records.push_back(record);

    StoredWriter stored;
    stored.writer = writer("codec-writer");
    stored.high_water = MutationSequence(1);
    stored.low_water = MutationSequence(1);
    StoredIdempotency idempotency;
    idempotency.sequence = MutationSequence(1);
    idempotency.key = "codec-key";
    idempotency.outcome_kind = StoredOutcomeKind::RegisteredAsset;
    idempotency.revision = 1;
    idempotency.asset = record->id;
    stored.records.push_back(idempotency);
    payload.writers.push_back(stored);

    auto encoded = encode_store_payload(payload, default_limits().max_document_bytes);
    AR_REQUIRE_OK(buffer, encoded);

    DecodeLimits decode_limits;
    decode_limits.limits = default_limits();
    decode_limits.max_bytes = default_limits().max_document_bytes;
    decode_limits.max_depth = default_limits().max_traversal_depth;
    auto decoded = decode_store_payload(buffer, decode_limits);
    AR_REQUIRE_OK(decoded_payload, decoded);

    AR_CHECK(decoded_payload.sequence == payload.sequence);
    AR_CHECK(decoded_payload.epoch == payload.epoch);
    AR_CHECK(decoded_payload.last_writer == payload.last_writer);
    AR_CHECK(decoded_payload.predecessor_sequence == payload.predecessor_sequence);
    AR_CHECK(decoded_payload.predecessor_crc == payload.predecessor_crc);
    AR_CHECK(decoded_payload.records.size() == 1);
    AR_REQUIRE_CHECK(decoded_payload.records.front() != nullptr);
    const std::shared_ptr<const AssetRecord> round_tripped = decoded_payload.records.front();
    AR_CHECK(round_tripped->id == record->id);
    AR_CHECK(round_tripped->asset_class == record->asset_class);
    AR_CHECK(round_tripped->serial_identity == record->serial_identity);
    AR_CHECK(round_tripped->metadata == record->metadata);
    AR_CHECK(round_tripped->references == record->references);
    AR_CHECK(round_tripped->state == record->state);
    AR_CHECK(round_tripped->provenance.size() == 1);
    AR_CHECK(round_tripped->provenance.front().action == ProvenanceAction::Registered);
    AR_CHECK(round_tripped->provenance.front().actor.to_string() == "codec-writer");
    AR_CHECK(round_tripped->provenance.front().recorded_at.has_value());
    AR_CHECK(round_tripped->provenance.front().recorded_at->unix_nanos() == 1'700'000'000'000'000'000LL);
    AR_CHECK(decoded_payload.writers.size() == 1);
    AR_CHECK(decoded_payload.writers.front().writer == stored.writer);
    AR_CHECK(decoded_payload.writers.front().records.size() == 1);
    AR_CHECK(decoded_payload.writers.front().records.front().key == "codec-key");
}

AR_TEST(codec, durable_payload_encoding_is_deterministic) {
    StorePayload payload;
    payload.sequence = TransactionSequence(9);
    payload.epoch = RegistryEpoch(3);
    payload.limits = default_limits();
    payload.last_writer = "determinism-writer";

    auto first = encode_store_payload(payload, default_limits().max_document_bytes);
    AR_REQUIRE_OK(first_buffer, first);
    auto second = encode_store_payload(payload, default_limits().max_document_bytes);
    AR_REQUIRE_OK(second_buffer, second);
    AR_CHECK(first_buffer == second_buffer);
    AR_CHECK(!first_buffer.empty());

    // Re-encoding the decoded payload reproduces the same bytes, so the durable form is a
    // fixed point of the codec rather than merely decodable.
    DecodeLimits decode_limits;
    decode_limits.limits = default_limits();
    decode_limits.max_bytes = default_limits().max_document_bytes;
    decode_limits.max_depth = default_limits().max_traversal_depth;
    auto decoded = decode_store_payload(first_buffer, decode_limits);
    AR_REQUIRE_OK(decoded_payload, decoded);
    auto reencoded = encode_store_payload(decoded_payload, default_limits().max_document_bytes);
    AR_REQUIRE_OK(reencoded_buffer, reencoded);
    AR_CHECK(reencoded_buffer == first_buffer);
}

AR_TEST(codec, an_empty_payload_round_trips) {
    StorePayload payload;
    payload.sequence = TransactionSequence(0);
    payload.epoch = RegistryEpoch(1);
    payload.limits = default_limits();

    auto encoded = encode_store_payload(payload, default_limits().max_document_bytes);
    AR_REQUIRE_OK(buffer, encoded);

    DecodeLimits decode_limits;
    decode_limits.limits = default_limits();
    decode_limits.max_bytes = default_limits().max_document_bytes;
    decode_limits.max_depth = default_limits().max_traversal_depth;
    auto decoded = decode_store_payload(buffer, decode_limits);
    AR_REQUIRE_OK(result, decoded);
    AR_CHECK(result.records.empty());
    AR_CHECK(result.writers.empty());
    AR_CHECK(result.epoch.value() == 1);
    // The root element is the payload list: the tag, then the payload length, then the element
    // count. Every payload carries the same nine members, and the first of them is the
    // transaction sequence.
    AR_CHECK_MSG(hex_of(buffer).rfind("8102", 0) == 0, "root tag " + hex_of(buffer).substr(0, 8));
    AR_CHECK_MSG(hex_of(buffer).substr(4, 4) == "a001", "root length " + hex_of(buffer).substr(0, 16));
    AR_CHECK_MSG(hex_of(buffer).substr(8, 2) == "09", "root member count " + hex_of(buffer).substr(0, 16));
    AR_CHECK_MSG(hex_of(buffer).substr(10, 4) == "8202", "root first member " + hex_of(buffer).substr(0, 16));
}

AR_TEST(codec, payload_damage_is_detected) {
    StorePayload payload;
    payload.sequence = TransactionSequence(1);
    payload.epoch = RegistryEpoch(1);
    payload.limits = default_limits();
    auto encoded = encode_store_payload(payload, default_limits().max_document_bytes);
    AR_REQUIRE_OK(buffer, encoded);

    DecodeLimits decode_limits;
    decode_limits.limits = default_limits();
    decode_limits.max_bytes = default_limits().max_document_bytes;
    decode_limits.max_depth = default_limits().max_traversal_depth;

    // Every truncation is rejected.
    for (std::size_t length = 0; length < buffer.size(); ++length) {
        const auto decoded = decode_store_payload(buffer.substr(0, length), decode_limits);
        AR_CHECK_MSG(!decoded.has_value(), "length " + std::to_string(length));
    }
    // Trailing bytes are rejected.
    const auto with_trailer = decode_store_payload(buffer + "x", decode_limits);
    AR_CHECK(!with_trailer.has_value());

    // A payload is not required to reject every bit flip on its own: the store verifies a
    // CRC over these bytes before it ever calls this decoder. What is required is that no
    // corruption is accepted silently as a different, still-valid payload.
    std::size_t accepted = 0;
    for (std::size_t position = 0; position < buffer.size(); ++position) {
        std::string damaged = buffer;
        damaged[position] = static_cast<char>(damaged[position] ^ 0x80);
        const auto decoded = decode_store_payload(damaged, decode_limits);
        if (decoded.has_value()) {
            ++accepted;
        }
    }
    log_line("payload corruption: " + std::to_string(accepted) + " of " + std::to_string(buffer.size()) +
             " single-bit flips accepted by the structural decoder alone");
    AR_CHECK(accepted < buffer.size());
}
