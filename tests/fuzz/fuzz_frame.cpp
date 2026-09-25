#include <braurser/protocol/frame.hpp>
#include <braurser/protocol/rpc.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    braurser::protocol::FrameDecoder decoder;
    for (std::size_t i = 0; i < size;) {
        const std::size_t count = std::min<std::size_t>(size - i, 1 + (data[i] % 64U));
        decoder.feed(std::as_bytes(std::span(data + i, count)));
        i += count;
        while (auto frame = decoder.next()) {
            if (!frame->has_value()) break;
            static_cast<void>(braurser::protocol::parse_message(**frame));
        }
    }
    static_cast<void>(braurser::protocol::parse_message(
        std::string_view(reinterpret_cast<const char*>(data), size)));
    return 0;
}
