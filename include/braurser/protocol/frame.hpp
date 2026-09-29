#ifndef BRAURSER_PROTOCOL_FRAME_HPP
#define BRAURSER_PROTOCOL_FRAME_HPP

#include <braurser/protocol/error.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::protocol {

inline constexpr std::size_t kMaxFrameBytes = 1024 * 1024;

/// in: json is UTF-8 JSON text no larger than kMaxFrameBytes.
/// out: a big-endian length-prefixed frame.
/// err: std::invalid_argument for empty, oversized or invalid UTF-8 input.
std::string encode_frame(std::string_view json);

class FrameDecoder {
public:
    /// in: data contains the next bytes of the framed stream.
    /// out: bytes are accepted up to the bounded frame limit.
    void feed(std::span<const std::byte> data);

    /// out: one complete JSON text, no result until more bytes arrive, or a fatal framing error.
    std::expected<std::optional<std::string>, ProtocolError> next();

private:
    std::vector<std::byte> buffer_;
    std::size_t next_header_offset_ = 0;
    std::size_t complete_bytes_ = 0;
    std::optional<ProtocolError> error_;
};

}

#endif
