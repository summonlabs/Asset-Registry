// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "tlv.hpp"

#include "asset_registry/text.hpp"

namespace asset_registry::internal {
namespace {

constexpr std::size_t kMaxVarintBytes = 10;

/// Smallest width that holds `value`, from the fixed set the format allows.
[[nodiscard]] std::size_t minimal_integer_width(std::uint64_t value) noexcept {
    if (value <= 0xFFULL) {
        return 1;
    }
    if (value <= 0xFFFFULL) {
        return 2;
    }
    if (value <= 0xFFFFFFFFULL) {
        return 4;
    }
    return 8;
}

}  // namespace

std::string_view to_string(TlvStatus status) noexcept {
    switch (status) {
        case TlvStatus::Ok:
            return "ok";
        case TlvStatus::UnexpectedEnd:
            return "unexpected_end_of_input";
        case TlvStatus::LengthOverflow:
            return "declared_length_exceeds_remaining_input";
        case TlvStatus::LengthMismatch:
            return "declared_length_does_not_match_consumed_bytes";
        case TlvStatus::TrailingData:
            return "trailing_bytes_after_last_element";
        case TlvStatus::WrongType:
            return "unexpected_element_tag";
        case TlvStatus::IntegerOverflow:
            return "integer_exceeds_permitted_range";
        case TlvStatus::IntegerOverflowedWidth:
            return "integer_encoded_wider_than_necessary";
        case TlvStatus::NotCanonical:
            return "non_canonical_encoding";
        case TlvStatus::DepthExceeded:
            return "nesting_depth_exceeded";
        case TlvStatus::CountExceeded:
            return "declared_count_exceeds_permitted_bound";
        case TlvStatus::EnumValueInvalid:
            return "enum_value_outside_domain";
        case TlvStatus::InvalidUtf8:
            return "string_is_not_valid_utf8";
        case TlvStatus::MismatchedTag:
            return "required_element_absent";
        case TlvStatus::UnclosedElement:
            return "element_left_open_by_writer";
        case TlvStatus::UnconsumedPayload:
            return "element_payload_was_not_consumed";
        case TlvStatus::ElementCountMismatch:
            return "list_declared_a_different_element_count";
        case TlvStatus::TagOutOfRange:
            return "tag_out_of_range";
    }
    return "unknown_codec_status";
}

std::size_t varint_size(std::uint64_t value) noexcept {
    std::size_t size = 1;
    while (value >= 0x80U) {
        value >>= 7U;
        ++size;
    }
    return size;
}

// ---------------------------------------------------------------------------
// TlvReader
// ---------------------------------------------------------------------------

bool TlvReader::read_varint(std::uint64_t& value) noexcept {
    value = 0;
    unsigned shift = 0;
    for (std::size_t index = 0; index < kMaxVarintBytes; ++index) {
        if (pos_ >= buffer_.size()) {
            fail(TlvStatus::UnexpectedEnd);
            return false;
        }
        const auto byte = static_cast<std::uint8_t>(buffer_[pos_++]);
        const std::uint64_t payload = static_cast<std::uint64_t>(byte & 0x7FU);
        if (shift > 63 || (shift == 63 && payload > 1)) {
            fail(TlvStatus::IntegerOverflow);
            return false;
        }
        value |= payload << shift;
        if ((byte & 0x80U) == 0) {
            // Only the final byte of a multi-byte varint may be zero: a zero in a continuation
            // position is a longer-than-necessary encoding.
            if (index > 0 && payload == 0) {
                fail(TlvStatus::NotCanonical);
                return false;
            }
            return true;
        }
        shift += 7;
    }
    fail(TlvStatus::IntegerOverflow);
    return false;
}

bool TlvReader::next() noexcept {
    if (!ok() || finished_) {
        return false;
    }
    if (current_.has_value() && !current_->is_list) {
        // A scalar leaves the traversal at the end of its own payload. A list element is followed
        // by its children instead, and the level it opened records where the traversal resumes
        // once those children have been read.
        if (levels_.empty()) {
            pos_ = current_->frame_end;
        } else {
            levels_.back().next_child = current_->frame_end;
        }
    }
    // Leave every list whose children have all been read. Popping a level carries the traversal
    // to the end of the list element's frame, which is where that element's next sibling begins.
    while (!levels_.empty() && levels_.back().remaining == 0) {
        const std::size_t after = levels_.back().frame_end;
        levels_.pop_back();
        if (!levels_.empty()) {
            levels_.back().next_child = after;
        }
        pos_ = after;
    }
    if (!levels_.empty()) {
        Level& level = levels_.back();
        pos_ = level.next_child;
        return read_child(level);
    }
    if (pos_ >= buffer_.size()) {
        current_ = std::nullopt;
        finished_ = true;
        return false;
    }
    return read_toplevel();
}

bool TlvReader::read_header(Frame& frame) noexcept {
    std::uint64_t tag = 0;
    if (!read_varint(tag)) {
        return false;
    }
    std::uint64_t length = 0;
    if (!read_varint(length)) {
        return false;
    }
    if (length > buffer_.size() - pos_) {
        fail(TlvStatus::LengthOverflow);
        return false;
    }
    frame.tag = tag;
    frame.payload_offset = pos_;
    frame.payload_size = static_cast<std::size_t>(length);
    frame.frame_end = pos_ + frame.payload_size;
    return true;
}

bool TlvReader::read_child(Level& level) noexcept {
    Frame frame;
    if (!read_header(frame)) {
        return false;
    }
    // A child must lie inside the region its parent declares for children, so a crafted length
    // cannot make a read see bytes belonging to a sibling level or to a later element.
    if (frame.frame_end > level.children_end) {
        fail(TlvStatus::LengthOverflow);
        return false;
    }
    // Reading the element consumes one of the list's declared children, and the list continues
    // after that child unless the child turns out to be a list of its own.
    level.remaining = level.remaining - 1;
    level.next_child = frame.frame_end;
    current_ = frame;
    return true;
}

bool TlvReader::read_toplevel() noexcept {
    Frame frame;
    if (!read_header(frame)) {
        return false;
    }
    current_ = frame;
    return true;
}

bool TlvReader::expect(std::uint64_t expected) noexcept {
    if (!current_.has_value()) {
        fail(TlvStatus::MismatchedTag);
        return false;
    }
    if (current_->tag != expected) {
        fail(TlvStatus::WrongType);
        return false;
    }
    return true;
}

bool TlvReader::read_unsigned(std::uint64_t& value) noexcept {
    value = 0;
    if (!current_.has_value()) {
        fail(TlvStatus::MismatchedTag);
        return false;
    }
    const std::size_t width = current_->payload_size;
    const std::size_t available =
        current_->payload_offset <= buffer_.size() ? buffer_.size() - current_->payload_offset : 0;
    if (width == 0 || width > available) {
        fail(TlvStatus::UnexpectedEnd);
        return false;
    }
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        fail(width > 8 ? TlvStatus::IntegerOverflow : TlvStatus::IntegerOverflowedWidth);
        return false;
    }
    for (std::size_t index = 0; index < width; ++index) {
        value = (value << 8U) |
                static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer_[current_->payload_offset + index]));
    }
    // An integer whose encoding is wider than the value needs has a second encoding, so it is
    // rejected: the format gives every value exactly one representation.
    if (width != minimal_integer_width(value)) {
        fail(TlvStatus::IntegerOverflowedWidth);
        return false;
    }
    pos_ = current_->payload_offset + width;
    current_->consumed = true;
    return true;
}

bool TlvReader::read_unsigned_checked(std::uint64_t maximum, std::uint64_t& value) noexcept {
    if (!read_unsigned(value)) {
        return false;
    }
    if (value > maximum) {
        fail(TlvStatus::IntegerOverflow);
        return false;
    }
    return true;
}

bool TlvReader::read_bool(bool& value) noexcept {
    value = false;
    if (!current_.has_value()) {
        fail(TlvStatus::MismatchedTag);
        return false;
    }
    if (current_->payload_size != 1) {
        fail(TlvStatus::IntegerOverflowedWidth);
        return false;
    }
    if (current_->payload_offset + 1 > buffer_.size()) {
        fail(TlvStatus::UnexpectedEnd);
        return false;
    }
    const auto raw = static_cast<std::uint8_t>(buffer_[current_->payload_offset]);
    if (raw > 1) {
        fail(TlvStatus::EnumValueInvalid);
        return false;
    }
    value = raw == 1;
    pos_ = current_->payload_offset + 1;
    current_->consumed = true;
    return true;
}

bool TlvReader::read_string_view(std::string_view& value) noexcept {
    value = {};
    if (!current_.has_value()) {
        fail(TlvStatus::MismatchedTag);
        return false;
    }
    if (current_->payload_offset > buffer_.size() ||
        current_->payload_size > buffer_.size() - current_->payload_offset) {
        fail(TlvStatus::UnexpectedEnd);
        return false;
    }
    value = buffer_.substr(current_->payload_offset, current_->payload_size);
    pos_ = current_->payload_offset + current_->payload_size;
    current_->consumed = true;
    return true;
}

bool TlvReader::read_string(std::string& value) {
    std::string_view view;
    if (!read_string_view(view)) {
        return false;
    }
    if (!view.empty() && validate_utf8(view, true) != Utf8Status::Valid) {
        fail(TlvStatus::InvalidUtf8);
        return false;
    }
    value.assign(view);
    return true;
}

bool TlvReader::read_bytes(std::vector<std::uint8_t>& value) {
    std::string_view view;
    if (!read_string_view(view)) {
        return false;
    }
    value.assign(view.begin(), view.end());
    return true;
}

bool TlvReader::read_list(std::uint64_t maximum, std::uint64_t& count) noexcept {
    count = 0;
    if (!current_.has_value()) {
        fail(TlvStatus::MismatchedTag);
        return false;
    }
    // A list payload begins with its element count, so a list payload is never empty. An element
    // with an empty payload is a scalar, and reading it as a list is a type error rather than an
    // empty list.
    if (current_->payload_size == 0) {
        fail(TlvStatus::WrongType);
        return false;
    }
    if (levels_.size() + 1 > max_depth_) {
        fail(TlvStatus::DepthExceeded);
        return false;
    }
    // The count is the first byte of the element's payload.
    pos_ = current_->payload_offset;
    std::uint64_t declared = 0;
    if (!read_varint(declared)) {
        return false;
    }
    // The count is part of the payload it introduces, so a count whose varint reaches past the
    // declared payload end is not a count at all: the element is malformed. Rejecting it here also
    // keeps the child region below from underflowing.
    const std::size_t count_width = pos_ - current_->payload_offset;
    if (count_width > current_->payload_size) {
        fail(TlvStatus::LengthOverflow);
        return false;
    }
    if (declared > maximum) {
        fail(TlvStatus::CountExceeded);
        return false;
    }
    // The children follow the count, and the element's declared payload length counts the count
    // itself, so the region the children occupy is what the payload holds after that count. The
    // child region therefore ends where the payload ends: a child can never reach past the element
    // that declares it, into a sibling of that element.
    const std::size_t children_offset = current_->payload_offset + count_width;
    const std::size_t children_end = current_->payload_offset + current_->payload_size;
    const std::size_t children_bytes = children_end - children_offset;
    // Every child occupies at least two bytes (a tag and a zero-length payload), so a count larger
    // than half the children's region can never be satisfied. This runs before the caller
    // allocates anything for the declared count.
    if (declared > static_cast<std::uint64_t>(children_bytes / 2U)) {
        fail(TlvStatus::CountExceeded);
        return false;
    }
    if (declared == 0 && children_bytes != 0) {
        // A list that declares no children but still carries bytes is malformed.
        fail(TlvStatus::LengthMismatch);
        return false;
    }
    Level level;
    level.payload_offset = current_->payload_offset;
    level.children_offset = children_offset;
    level.children_end = children_end;
    level.frame_end = current_->payload_offset + current_->payload_size;
    level.next_child = children_offset;
    level.remaining = declared;
    // The element is a list: its payload is consumed by its children, so the enclosing level is
    // not advanced past it when it is read. Reading the list element has already consumed one
    // child of that enclosing level, but the enclosing level's position must follow the whole
    // element, which is what is recorded here.
    const bool nested = !levels_.empty();
    current_->is_list = true;
    current_->consumed = true;
    if (nested) {
        levels_.back().next_child = level.frame_end;
    }
    // Entering the list pushes its level even when it declares no children, so every list the
    // traversal enters is a level it must leave again. That is what keeps an empty list and a
    // populated one on the same footing: the cursor after read_list() is the position the count
    // ends at, the next next() moves past the whole element (the level is spent, so it is popped
    // and the cursor lands on the end of the element's frame), and a leave_list() after it leaves
    // this list rather than the list that encloses it.
    levels_.push_back(level);
    pos_ = children_offset;
    count = declared;
    return true;
}

void TlvReader::leave_list() noexcept {
    if (levels_.empty()) {
        return;
    }
    // Abandon the innermost list, and with it every list nested inside it: the traversal resumes
    // at the end of that list element's frame, which is where its next sibling begins. The
    // enclosing levels are dropped rather than resumed, because the caller has stated that the
    // list is finished and the enclosing levels would otherwise continue inside it. The current
    // element goes too, so the next step reads from the cursor rather than finishing an element
    // that has already been left behind.
    const std::size_t after = levels_.back().frame_end;
    levels_.clear();
    current_ = std::nullopt;
    pos_ = after;
}

void TlvReader::skip() noexcept {
    if (!current_.has_value()) {
        return;
    }
    // Nothing inside the element is read, so the traversal resumes at the end of the element,
    // whatever it contained. Any level the element opened is abandoned with it.
    const std::size_t after = current_->frame_end;
    levels_.clear();
    current_ = std::nullopt;
    pos_ = after;
    if (after >= buffer_.size()) {
        finished_ = true;
    }
}

// ---------------------------------------------------------------------------
// TlvWriter
// ---------------------------------------------------------------------------

void TlvWriter::append_varint(std::string& target, std::uint64_t value) {
    while (value >= 0x80U) {
        target.push_back(static_cast<char>(static_cast<std::uint8_t>(value) | 0x80U));
        value >>= 7U;
    }
    target.push_back(static_cast<char>(static_cast<std::uint8_t>(value)));
}

void TlvWriter::append_unsigned(std::uint64_t tag, std::uint64_t value) {
    const std::size_t width = minimal_integer_width(value);
    Frame& frame = stack_.back();
    append_varint(frame.bytes, tag);
    append_varint(frame.bytes, width);
    for (std::size_t index = 0; index < width; ++index) {
        const std::size_t shift = (width - 1 - index) * 8U;
        frame.bytes.push_back(static_cast<char>(static_cast<std::uint8_t>((value >> shift) & 0xFFU)));
    }
    ++frame.written;
}

void TlvWriter::append_bool(std::uint64_t tag, bool value) {
    Frame& frame = stack_.back();
    append_varint(frame.bytes, tag);
    append_varint(frame.bytes, 1);
    frame.bytes.push_back(static_cast<char>(value ? 1 : 0));
    ++frame.written;
}

void TlvWriter::append_string(std::uint64_t tag, std::string_view value) {
    Frame& frame = stack_.back();
    append_varint(frame.bytes, tag);
    append_varint(frame.bytes, value.size());
    frame.bytes.append(value.data(), value.size());
    ++frame.written;
}

void TlvWriter::append_bytes(std::uint64_t tag, const std::uint8_t* data, std::size_t size) {
    Frame& frame = stack_.back();
    append_varint(frame.bytes, tag);
    append_varint(frame.bytes, size);
    for (std::size_t index = 0; index < size; ++index) {
        frame.bytes.push_back(static_cast<char>(data[index]));
    }
    ++frame.written;
}

void TlvWriter::open_list(std::uint64_t tag, std::uint64_t count) {
    (void)count;
    Frame frame;
    frame.tag = tag;
    stack_.push_back(std::move(frame));
}

void TlvWriter::close_list() {
    if (stack_.size() < 2) {
        return;
    }
    Frame frame = std::move(stack_.back());
    stack_.pop_back();

    // The count written is the number of elements actually appended while the list was open, not
    // the number the caller announced. The two agree for a well-formed caller, and deriving the
    // count from what was written means a caller that miscounts still produces a payload whose
    // count matches its contents.
    std::string payload;
    append_varint(payload, frame.written);
    payload += frame.bytes;

    Frame& parent = stack_.back();
    append_varint(parent.bytes, frame.tag);
    append_varint(parent.bytes, payload.size());
    parent.bytes += payload;
    ++parent.written;
}

}  // namespace asset_registry::internal
