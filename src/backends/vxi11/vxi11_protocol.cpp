#include "backends/vxi11/vxi11_protocol.h"

#include <limits>

namespace wrvisa::vxi11 {
namespace {

std::uint32_t read_u32(const std::uint8_t* input) noexcept {
    return (static_cast<std::uint32_t>(input[0]) << 24u) |
           (static_cast<std::uint32_t>(input[1]) << 16u) |
           (static_cast<std::uint32_t>(input[2]) << 8u) |
           static_cast<std::uint32_t>(input[3]);
}

}  // namespace

void XdrWriter::u32(std::uint32_t value) {
    bytes_.push_back(static_cast<std::uint8_t>(value >> 24u));
    bytes_.push_back(static_cast<std::uint8_t>(value >> 16u));
    bytes_.push_back(static_cast<std::uint8_t>(value >> 8u));
    bytes_.push_back(static_cast<std::uint8_t>(value));
}

void XdrWriter::boolean(bool value) { u32(value ? 1u : 0u); }

void XdrWriter::opaque(std::span<const std::uint8_t> value) {
    u32(static_cast<std::uint32_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    while ((bytes_.size() & 3u) != 0u) {
        bytes_.push_back(0);
    }
}

void XdrWriter::string(std::string_view value) {
    opaque(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

bool XdrReader::u32(std::uint32_t& value) {
    if (bytes_.size() - offset_ < 4u) {
        return false;
    }
    value = read_u32(bytes_.data() + offset_);
    offset_ += 4u;
    return true;
}

bool XdrReader::opaque(std::vector<std::uint8_t>& value, std::size_t maximum) {
    std::uint32_t size = 0;
    if (!u32(size) || size > maximum) {
        return false;
    }
    const auto padded = (static_cast<std::size_t>(size) + 3u) & ~std::size_t{3u};
    if (padded > bytes_.size() - offset_) {
        return false;
    }
    value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                 bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
    offset_ += padded;
    return true;
}

std::vector<std::uint8_t> make_rpc_call(std::uint32_t xid,
                                        std::uint32_t program,
                                        std::uint32_t version,
                                        std::uint32_t procedure,
                                        std::span<const std::uint8_t> arguments) {
    XdrWriter message;
    message.u32(xid);
    message.u32(0);  // CALL
    message.u32(2);  // RPC version 2
    message.u32(program);
    message.u32(version);
    message.u32(procedure);
    message.u32(0);  // AUTH_NONE credential
    message.u32(0);
    message.u32(0);  // AUTH_NONE verifier
    message.u32(0);
    auto body = message.take();
    body.insert(body.end(), arguments.begin(), arguments.end());
    if (body.size() > static_cast<std::size_t>(UINT32_C(0x7FFFFFFF))) {
        return {};
    }
    std::vector<std::uint8_t> framed;
    framed.reserve(body.size() + 4u);
    const auto marker = UINT32_C(0x80000000) |
                        static_cast<std::uint32_t>(body.size());
    framed.push_back(static_cast<std::uint8_t>(marker >> 24u));
    framed.push_back(static_cast<std::uint8_t>(marker >> 16u));
    framed.push_back(static_cast<std::uint8_t>(marker >> 8u));
    framed.push_back(static_cast<std::uint8_t>(marker));
    framed.insert(framed.end(), body.begin(), body.end());
    return framed;
}

bool parse_rpc_reply(std::span<const std::uint8_t> message,
                     std::uint32_t expected_xid,
                     std::span<const std::uint8_t>& result) {
    XdrReader reader(message);
    std::uint32_t xid = 0;
    std::uint32_t message_type = 0;
    std::uint32_t reply_status = 0;
    std::uint32_t verifier_flavor = 0;
    std::uint32_t verifier_size = 0;
    if (!reader.u32(xid) || !reader.u32(message_type) ||
        !reader.u32(reply_status) || xid != expected_xid || message_type != 1u ||
        reply_status != 0u || !reader.u32(verifier_flavor) ||
        !reader.u32(verifier_size)) {
        return false;
    }
    static_cast<void>(verifier_flavor);
    const auto verifier_padded =
        (static_cast<std::size_t>(verifier_size) + 3u) & ~std::size_t{3u};
    auto remaining = reader.remaining();
    if (verifier_padded > remaining.size()) {
        return false;
    }
    remaining = remaining.subspan(verifier_padded);
    XdrReader accepted(remaining);
    std::uint32_t accept_status = 0;
    if (!accepted.u32(accept_status) || accept_status != 0u) {
        return false;
    }
    result = accepted.remaining();
    return true;
}

}  // namespace wrvisa::vxi11
