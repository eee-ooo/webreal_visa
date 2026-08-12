#include "backends/hislip/hislip_protocol.h"

#include <limits>

namespace wrvisa::hislip {
namespace {

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24u));
    output.push_back(static_cast<std::uint8_t>(value >> 16u));
    output.push_back(static_cast<std::uint8_t>(value >> 8u));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    append_u32(output, static_cast<std::uint32_t>(value >> 32u));
    append_u32(output, static_cast<std::uint32_t>(value));
}

std::uint32_t read_u32(const std::uint8_t* input) noexcept {
    return (static_cast<std::uint32_t>(input[0]) << 24u) |
           (static_cast<std::uint32_t>(input[1]) << 16u) |
           (static_cast<std::uint32_t>(input[2]) << 8u) |
           static_cast<std::uint32_t>(input[3]);
}

std::uint64_t read_u64(const std::uint8_t* input) noexcept {
    return (static_cast<std::uint64_t>(read_u32(input)) << 32u) |
           static_cast<std::uint64_t>(read_u32(input + 4));
}

}  // namespace

std::vector<std::uint8_t> encode(MessageType type, std::uint8_t control,
                                 std::uint32_t parameter,
                                 std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(16u + payload.size());
    bytes.push_back('H');
    bytes.push_back('S');
    bytes.push_back(static_cast<std::uint8_t>(type));
    bytes.push_back(control);
    append_u32(bytes, parameter);
    append_u64(bytes, static_cast<std::uint64_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

bool decode(std::span<const std::uint8_t> bytes, Frame& frame) {
    if (bytes.size() < 16u || bytes[0] != 'H' || bytes[1] != 'S') {
        return false;
    }
    const auto payload_size = read_u64(bytes.data() + 8u);
    if (payload_size >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        bytes.size() - 16u != static_cast<std::size_t>(payload_size)) {
        return false;
    }
    frame.type = static_cast<MessageType>(bytes[2]);
    frame.control = bytes[3];
    frame.parameter = read_u32(bytes.data() + 4u);
    frame.payload.assign(bytes.begin() + 16, bytes.end());
    return true;
}

}  // namespace wrvisa::hislip
