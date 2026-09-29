#include <braurser/protocol/frame.hpp>
#include <braurser/protocol/dto.hpp>
#include <array>
#include <stdexcept>

namespace braurser::protocol {

std::string encode_frame(std::string_view json) {
    if (json.empty() || json.size() > kMaxFrameBytes || !valid_utf8(json)) {
        throw std::invalid_argument("invalid frame payload");
    }
    const auto size = static_cast<std::uint32_t>(json.size());
    std::string result;
    result.reserve(json.size() + 4);
    for (int shift : {24, 16, 8, 0}) {
        result.push_back(static_cast<char>((size >> shift) & 0xffU));
    }
    result.append(json);
    return result;
}

void FrameDecoder::feed(std::span<const std::byte> data) {
    if (error_) return;
    for (const std::byte byte : data) {
        if (buffer_.size() - complete_bytes_ >= kMaxFrameBytes + 4 ||
            buffer_.size() >= 2 * (kMaxFrameBytes + 4)) {
            error_ = ProtocolError{kInvalidRequest, "error.invalid_frame"};
            return;
        }
        buffer_.push_back(byte);
        if (buffer_.size() == next_header_offset_ && next_header_offset_ > complete_bytes_)
            complete_bytes_ = next_header_offset_;
        if (buffer_.size() == next_header_offset_ + 4) {
            std::uint32_t length = 0;
            for (std::size_t i = next_header_offset_; i < next_header_offset_ + 4; ++i) {
                length = (length << 8U) | std::to_integer<std::uint32_t>(buffer_[i]);
            }
            if (length == 0 || length > kMaxFrameBytes) {
                error_ = ProtocolError{kInvalidRequest, "error.invalid_frame"};
                return;
            }
            next_header_offset_ += static_cast<std::size_t>(length) + 4;
        }
    }
}

std::expected<std::optional<std::string>, ProtocolError> FrameDecoder::next() {
    if (error_) return std::unexpected(*error_);
    if (buffer_.size() < 4) return std::nullopt;
    std::uint32_t length = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        length = (length << 8U) | std::to_integer<std::uint32_t>(buffer_[i]);
    }
    if (length == 0 || length > kMaxFrameBytes) {
        error_ = ProtocolError{kInvalidRequest, "error.invalid_frame"};
        return std::unexpected(*error_);
    }
    if (buffer_.size() < static_cast<std::size_t>(length) + 4) return std::nullopt;
    std::string json;
    json.reserve(length);
    for (std::size_t i = 4; i < static_cast<std::size_t>(length) + 4; ++i) {
        json.push_back(static_cast<char>(std::to_integer<unsigned char>(buffer_[i])));
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(length + 4));
    next_header_offset_ -= static_cast<std::size_t>(length) + 4;
    complete_bytes_ -= static_cast<std::size_t>(length) + 4;
    if (!valid_utf8(json)) {
        error_ = ProtocolError{kParseError, "error.invalid_json"};
        return std::unexpected(*error_);
    }
    return json;
}

}
