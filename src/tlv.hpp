// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal: framed tag/length/value codec.
//
// Wire rules
// ----------
//   * Every element is: tag (varint), payload length (varint), payload bytes.
//   * Integers are big-endian with minimal width: 1, 2, 4, or 8 bytes. The width is
//     the smallest one that holds the value, so every value has exactly one encoding.
//   * A list is one element whose payload is: element count (varint) followed by
//     exactly that many nested elements.
//   * Varints are minimal. A non-minimal varint is rejected, so one value has exactly
//     one byte representation.
//
// Reader model
// ------------
// The reader keeps one cursor, `pos_`, which always holds the offset of the next byte
// the traversal will read. On top of it sit two pieces of state:
//
//   * `current_` is the element whose header was read last, together with the extent of
//     its payload and whether that payload has been accounted for.
//   * `levels_` is the stack of list payloads currently being iterated. Each level
//     records the extent of the list element's payload, the element count that
//     introduces it, and how many of those elements have been read.
//
// The traversal is a pre-order walk:
//
//   * `next()` finishes the current element, closes every level whose element budget is
//     spent, then reads the element header at the cursor.
//   * An element is finished when its payload has been accounted for. A scalar is
//     accounted for by the single read helper that consumed its payload; a list is
//     accounted for when all of its children have been read; an element that was
//     skipped is accounted for by `skip()`.
//   * `read_list()` consumes the element count from the current element's payload and
//     pushes a level, so the children of that list are what `next()` returns next.
//   * When a list's children are exhausted, its level is popped and the cursor is
//     placed at the end of the list element's payload, which is exactly where the next
//     sibling of that list begins.
//
// Because the cursor is placed by the traversal itself and the element count bounds how
// many elements a level can yield, an element can neither be read twice nor skipped
// silently. This is what stops a crafted payload from moving bytes out of one field and
// into another: every read is confined to the current element's declared payload, and
// every element is confined to the level that declares it.
//
// The decoder is the trust boundary for every durable artefact and every parsed import
// document. It never reads past its buffer, never allocates before a declared count and
// length have been validated against the remaining input, and reports a structured
// status instead of throwing.

#ifndef ASSET_REGISTRY_INTERNAL_TLV_HPP
#define ASSET_REGISTRY_INTERNAL_TLV_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asset_registry::internal {

enum class TlvStatus {
    Ok,
    UnexpectedEnd,
    LengthOverflow,
    LengthMismatch,
    TrailingData,
    WrongType,
    IntegerOverflow,
    IntegerOverflowedWidth,
    NotCanonical,
    DepthExceeded,
    CountExceeded,
    EnumValueInvalid,
    InvalidUtf8,
    MismatchedTag,
    UnclosedElement,
    UnconsumedPayload,
    ElementCountMismatch,
    TagOutOfRange,
};

[[nodiscard]] std::string_view to_string(TlvStatus status) noexcept;

class TlvReader {
public:
    TlvReader(std::string_view buffer, std::uint32_t max_depth) noexcept
        : buffer_(buffer), max_depth_(max_depth) {}

    /// Positions the reader at the first element, discarding any traversal state.
    void begin() noexcept {
        pos_ = 0;
        status_ = TlvStatus::Ok;
        finished_ = false;
        current_ = std::nullopt;
        levels_.clear();
    }

    [[nodiscard]] TlvStatus status() const noexcept { return status_; }
    [[nodiscard]] bool ok() const noexcept { return status_ == TlvStatus::Ok; }
    /// Offset of the byte a following read consumes.
    [[nodiscard]] std::size_t offset() const noexcept { return pos_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return buffer_.size() - pos_; }
    /// Nesting depth of the current element: zero at the outermost level.
    [[nodiscard]] std::uint32_t depth() const noexcept { return static_cast<std::uint32_t>(levels_.size()); }

    /// Advances to the next element. Returns false at the end of the traversal or on a
    /// malformed payload; the two are distinguished by ok().
    [[nodiscard]] bool next() noexcept;

    /// True when every byte of the payload has been accounted for and no error occurred: either
    /// the traversal ran out of elements, or it stands at the end of the input after the last
    /// element whose frame ended there.
    [[nodiscard]] bool exhausted() const noexcept { return ok() && (finished_ || pos_ == buffer_.size()); }
    /// True when the traversal ran out of elements with no error.
    [[nodiscard]] bool complete() const noexcept { return ok() && finished_; }

    [[nodiscard]] std::uint64_t tag() const noexcept { return current_.has_value() ? current_->tag : 0; }
    [[nodiscard]] std::uint64_t payload_size() const noexcept {
        return current_.has_value() ? static_cast<std::uint64_t>(current_->payload_size) : 0;
    }

    /// Fails with WrongType unless the current element tag equals `expected`.
    [[nodiscard]] bool expect(std::uint64_t expected) noexcept;

    [[nodiscard]] bool read_unsigned(std::uint64_t& value) noexcept;
    [[nodiscard]] bool read_unsigned_checked(std::uint64_t maximum, std::uint64_t& value) noexcept;
    [[nodiscard]] bool read_bool(bool& value) noexcept;
    [[nodiscard]] bool read_string(std::string& value);
    [[nodiscard]] bool read_string_view(std::string_view& value) noexcept;
    [[nodiscard]] bool read_bytes(std::vector<std::uint8_t>& value);

    /// Consumes the current element's element count and enters the list, so the
    /// following next() calls yield its children. The count is validated against
    /// `maximum` and against the remaining payload before it is returned. Fails with
    /// WrongType when the current element cannot be a list: a list payload always begins
    /// with a count, so an element with an empty payload is a scalar.
    [[nodiscard]] bool read_list(std::uint64_t maximum, std::uint64_t& count) noexcept;

    /// Abandons the current list without reading its remaining children. The traversal
    /// resumes after the list element, so the caller sees the next sibling.
    void leave_list() noexcept;

    /// True once the traversal has run out of elements. next() returns false with this set
    /// when the payload ended cleanly, and returns false with ok() false when it did not.
    [[nodiscard]] bool at_end() const noexcept { return finished_; }

    /// Skips the payload of the current element, however deeply it is nested.
    void skip() noexcept;

private:
    struct Frame {
        std::uint64_t tag = 0;
        /// Payload extent of the current element.
        std::size_t payload_offset = 0;
        std::size_t payload_size = 0;
        /// End offset of the whole element, header included. It is where the element's
        /// next sibling begins.
        std::size_t frame_end = 0;
        /// False until the payload has been accounted for, either by a read helper, by
        /// reading all of a list's children, or by skip().
        bool consumed = false;
        /// Set when the element is entered as a list. A list is consumed by its children,
        /// so the enclosing level does not advance past it when it is read.
        bool is_list = false;
    };

    /// One list payload being iterated.
    struct Level {
        /// Start of the list element's payload, which is the count that introduces it.
        std::size_t payload_offset = 0;
        /// Offset of the first child, immediately after the element count.
        std::size_t children_offset = 0;
        /// One past the last byte a child may occupy: the end of the element's declared payload,
        /// which is where the last child ends. The element count sits inside that payload, before
        /// the child region, so the region a child may occupy is the payload minus that count.
        std::size_t children_end = 0;
        /// End of the whole list element, header included. It is where the next sibling of
        /// the list element begins.
        std::size_t frame_end = 0;
        /// Offset of the next child to read.
        std::size_t next_child = 0;
        /// How many children are still to be read.
        std::uint64_t remaining = 0;
    };

    [[nodiscard]] bool read_varint(std::uint64_t& value) noexcept;
    /// Reads one element header at the cursor.
    [[nodiscard]] bool read_header(Frame& frame) noexcept;
    /// Reads the next child of `level` at the cursor, bounded by that level.
    [[nodiscard]] bool read_child(Level& level) noexcept;
    /// Reads the next element at the outermost level, at the cursor.
    [[nodiscard]] bool read_toplevel() noexcept;

    void fail(TlvStatus status) noexcept { status_ = status; }

    std::string_view buffer_;
    /// Offset of the next byte the traversal reads.
    std::size_t pos_ = 0;

    std::uint32_t max_depth_ = 32;
    TlvStatus status_ = TlvStatus::Ok;
    /// Set once the traversal has run out of elements.
    bool finished_ = false;

    std::optional<Frame> current_;
    /// Enclosing list payloads, outermost first.
    std::vector<Level> levels_;
};

/// Encoder for the same wire format.
///
/// The writer is a stack of accumulators. Opening a list pushes a fresh buffer, so the
/// elements written while that list is open go into the list's own payload and nowhere
/// else; closing the list emits its tag, its exact payload length, the element count,
/// and the accumulated payload, then pops. Because a list's payload is assembled before
/// its length is written, the length is always exact and the encoding is always
/// minimal, with no placeholder to patch and no way for a nested list to be spliced
/// into its parent's payload.
class TlvWriter {
public:
    TlvWriter() { stack_.push_back(Frame{}); }

    void append_unsigned(std::uint64_t tag, std::uint64_t value);
    void append_bool(std::uint64_t tag, bool value);
    void append_string(std::uint64_t tag, std::string_view value);
    void append_bytes(std::uint64_t tag, const std::uint8_t* data, std::size_t size);

    /// Opens a list element. The caller appends the nested elements and then calls
    /// close_list(), which writes the number of elements actually appended as the count.
    void open_list(std::uint64_t tag, std::uint64_t count);
    void close_list();

    [[nodiscard]] const std::string& buffer() const noexcept { return stack_.front().bytes; }
    [[nodiscard]] std::string take() { return std::move(stack_.front().bytes); }
    [[nodiscard]] std::size_t size() const noexcept { return stack_.front().bytes.size(); }
    [[nodiscard]] bool balanced() const noexcept { return stack_.size() == 1; }
    void reserve(std::size_t bytes) { stack_.front().bytes.reserve(bytes); }
    void clear() {
        stack_.clear();
        stack_.push_back(Frame{});
    }

private:
    struct Frame {
        std::string bytes;
        std::uint64_t tag = 0;
        /// The number of elements appended while this list was open.
        std::uint64_t written = 0;
    };

    void append_varint(std::string& target, std::uint64_t value);

    std::vector<Frame> stack_;
};

/// Number of bytes a minimal varint encoding of `value` occupies.
[[nodiscard]] std::size_t varint_size(std::uint64_t value) noexcept;

}  // namespace asset_registry::internal

#endif  // ASSET_REGISTRY_INTERNAL_TLV_HPP
