#include "backends/usb/usbtmc_protocol.h"

#include <limits>

namespace wrvisa::usbtmc {
namespace {

std::size_t padding_for(std::size_t payload_size) {
    return (4u - (payload_size % 4u)) % 4u;
}

void append_u32_le(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
}

std::uint32_t read_u32_le(std::span<const std::uint8_t> bytes,
                          std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

bool valid_common_header(std::span<const std::uint8_t> bytes,
                         std::uint8_t message_id, std::uint8_t expected_tag) {
    return bytes.size() >= kHeaderSize && bytes[0] == message_id &&
           bytes[1] != 0 && bytes[1] == expected_tag &&
           bytes[2] == static_cast<std::uint8_t>(~bytes[1]) && bytes[3] == 0;
}

std::optional<std::vector<std::uint8_t>> encode_message(
    std::uint8_t message_id, std::uint8_t tag,
    std::span<const std::uint8_t> payload, std::uint8_t attributes,
    std::uint8_t termchar) {
    if (tag == 0 || payload.size() > kMaximumPayload ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderSize + payload.size() + padding_for(payload.size()));
    bytes.push_back(message_id);
    bytes.push_back(tag);
    bytes.push_back(static_cast<std::uint8_t>(~tag));
    bytes.push_back(0);
    append_u32_le(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.push_back(attributes);
    bytes.push_back(termchar);
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    bytes.insert(bytes.end(), padding_for(payload.size()), 0);
    return bytes;
}

bool decode_message(std::span<const std::uint8_t> bytes,
                    std::uint8_t message_id, std::uint8_t expected_tag,
                    DevDepMessage& message, std::size_t maximum_payload,
                    std::size_t maximum_alignment, bool fixed_alignment) {
    if (!valid_common_header(bytes, message_id, expected_tag) ||
        (bytes[8] & 0xfeu) != 0 || bytes[9] != 0 || bytes[10] != 0 ||
        bytes[11] != 0) {
        return false;
    }
    const auto payload_size = static_cast<std::size_t>(read_u32_le(bytes, 4));
    if (payload_size > maximum_payload || payload_size > kMaximumPayload) {
        return false;
    }
    const auto content_size = kHeaderSize + payload_size;
    if (bytes.size() < content_size) {
        return false;
    }
    const auto alignment_size = bytes.size() - content_size;
    if ((fixed_alignment && alignment_size != padding_for(payload_size)) ||
        (!fixed_alignment && alignment_size > maximum_alignment)) {
        return false;
    }
    message.tag = bytes[1];
    message.end_of_message = (bytes[8] & 1u) != 0;
    message.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                           bytes.begin() + static_cast<std::ptrdiff_t>(
                                               kHeaderSize + payload_size));
    return true;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> encode_dev_dep_msg_out(
    std::uint8_t tag, std::span<const std::uint8_t> payload,
    bool end_of_message) {
    return encode_message(kDevDepMsgOut, tag, payload,
                          end_of_message ? 1u : 0u, 0);
}

std::optional<std::vector<std::uint8_t>> encode_request_dev_dep_msg_in(
    std::uint8_t tag, std::uint32_t transfer_size, bool termchar_enabled,
    std::uint8_t termchar) {
    if (tag == 0 || transfer_size == 0 || transfer_size > kMaximumPayload) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderSize);
    bytes.push_back(kRequestDevDepMsgIn);
    bytes.push_back(tag);
    bytes.push_back(static_cast<std::uint8_t>(~tag));
    bytes.push_back(0);
    append_u32_le(bytes, transfer_size);
    bytes.push_back(termchar_enabled ? 2u : 0u);
    bytes.push_back(termchar);
    bytes.push_back(0);
    bytes.push_back(0);
    return bytes;
}

std::optional<std::vector<std::uint8_t>> encode_dev_dep_msg_in(
    std::uint8_t tag, std::span<const std::uint8_t> payload,
    bool end_of_message) {
    return encode_message(kDevDepMsgIn, tag, payload,
                          end_of_message ? 1u : 0u, 0);
}

std::optional<std::vector<std::uint8_t>> encode_usb488_trigger(
    std::uint8_t tag) {
    if (tag == 0) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>{
        kUsb488Trigger, tag, static_cast<std::uint8_t>(~tag), 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
}

bool decode_dev_dep_msg_out(std::span<const std::uint8_t> bytes,
                            DevDepMessage& message,
                            std::size_t maximum_payload) {
    if (bytes.size() < kHeaderSize) {
        return false;
    }
    return decode_message(bytes, kDevDepMsgOut, bytes[1], message,
                          maximum_payload, 3, true);
}

bool decode_request_dev_dep_msg_in(std::span<const std::uint8_t> bytes,
                                   std::uint8_t& tag,
                                   std::uint32_t& transfer_size,
                                   bool& termchar_enabled,
                                   std::uint8_t& termchar) {
    if (bytes.size() != kHeaderSize ||
        !valid_common_header(bytes, kRequestDevDepMsgIn, bytes[1]) ||
        (bytes[8] & 0xfdu) != 0 || bytes[10] != 0 || bytes[11] != 0) {
        return false;
    }
    transfer_size = read_u32_le(bytes, 4);
    if (transfer_size == 0 || transfer_size > kMaximumPayload) {
        return false;
    }
    tag = bytes[1];
    termchar_enabled = (bytes[8] & 2u) != 0;
    termchar = bytes[9];
    return true;
}

bool decode_dev_dep_msg_in(std::span<const std::uint8_t> bytes,
                           std::uint8_t expected_tag, DevDepMessage& message,
                           std::size_t maximum_payload,
                           std::size_t maximum_alignment) {
    return decode_message(bytes, kDevDepMsgIn, expected_tag, message,
                          maximum_payload, maximum_alignment, false);
}

bool decode_usb488_trigger(std::span<const std::uint8_t> bytes,
                           std::uint8_t& tag) {
    if (bytes.size() != kHeaderSize ||
        !valid_common_header(bytes, kUsb488Trigger, bytes[1])) {
        return false;
    }
    for (std::size_t index = 4; index < bytes.size(); ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    tag = bytes[1];
    return true;
}

bool decode_capabilities(std::span<const std::uint8_t> bytes,
                         bool expect_usb488, Capabilities& capabilities) {
    if (bytes.size() != 24) {
        return false;
    }
    capabilities = {};
    capabilities.status = bytes[0];
    if (bytes[0] != kStatusSuccess) {
        return true;
    }
    const auto usbtmc_version = static_cast<std::uint16_t>(bytes[2]) |
                                (static_cast<std::uint16_t>(bytes[3]) << 8u);
    if (bytes[1] != 0 || usbtmc_version < 0x0100 ||
        (bytes[4] & 0xf8u) != 0 || (bytes[5] & 0xfeu) != 0) {
        return false;
    }
    for (std::size_t index = 6; index < 12; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    capabilities.termchar = (bytes[5] & 1u) != 0;
    if (!expect_usb488) {
        for (std::size_t index = 12; index < 24; ++index) {
            if (bytes[index] != 0) {
                return false;
            }
        }
        return true;
    }
    const auto usb488_version = static_cast<std::uint16_t>(bytes[12]) |
                                (static_cast<std::uint16_t>(bytes[13]) << 8u);
    if (usb488_version < 0x0100 || (bytes[14] & 0xf8u) != 0 ||
        (bytes[15] & 0xf0u) != 0) {
        return false;
    }
    for (std::size_t index = 16; index < 24; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    const bool trigger = (bytes[14] & 1u) != 0;
    const bool remote_local = (bytes[14] & 2u) != 0;
    const bool ieee488_2 = (bytes[14] & 4u) != 0;
    const bool device_trigger = (bytes[15] & 1u) != 0;
    const bool device_remote_local = (bytes[15] & 2u) != 0;
    const bool service_request = (bytes[15] & 4u) != 0;
    const bool scpi = (bytes[15] & 8u) != 0;
    if ((device_trigger && !trigger) ||
        (device_remote_local && !remote_local) ||
        (ieee488_2 && !service_request) ||
        (scpi && (!service_request || !ieee488_2))) {
        return false;
    }
    capabilities.usb488 = true;
    capabilities.usb488_trigger = trigger && device_trigger;
    capabilities.usb488_service_request = service_request;
    return true;
}

bool decode_initiate_abort(std::span<const std::uint8_t> bytes,
                           std::uint8_t expected_tag, std::uint8_t& status) {
    if (bytes.size() != 2) {
        return false;
    }
    status = bytes[0];
    return status != kStatusSuccess || bytes[1] == expected_tag;
}

bool decode_check_abort_bulk_out(std::span<const std::uint8_t> bytes,
                                 SplitStatus& result) {
    if (bytes.size() != 8) {
        return false;
    }
    result = {};
    result.status = bytes[0];
    if (bytes[0] == kStatusSuccess || bytes[0] == kStatusPending) {
        if (bytes[1] != 0 || bytes[2] != 0 || bytes[3] != 0) {
            return false;
        }
        result.transferred = read_u32_le(bytes, 4);
    }
    return true;
}

bool decode_check_abort_bulk_in(std::span<const std::uint8_t> bytes,
                                SplitStatus& result) {
    if (bytes.size() != 8) {
        return false;
    }
    result = {};
    result.status = bytes[0];
    if (bytes[0] == kStatusSuccess || bytes[0] == kStatusPending) {
        if ((bytes[1] & 0xfeu) != 0 || bytes[2] != 0 || bytes[3] != 0) {
            return false;
        }
        result.bulk_in_fifo_bytes = (bytes[1] & 1u) != 0;
        result.transferred = read_u32_le(bytes, 4);
        if ((bytes[0] == kStatusSuccess && result.bulk_in_fifo_bytes) ||
            (bytes[0] == kStatusPending && result.transferred != 0)) {
            return false;
        }
    }
    return true;
}

bool decode_initiate_clear(std::span<const std::uint8_t> bytes,
                           std::uint8_t& status) {
    if (bytes.size() != 1) {
        return false;
    }
    status = bytes[0];
    return true;
}

bool decode_check_clear(std::span<const std::uint8_t> bytes,
                        SplitStatus& result) {
    if (bytes.size() != 2) {
        return false;
    }
    result = {};
    result.status = bytes[0];
    if (bytes[0] == kStatusSuccess || bytes[0] == kStatusPending) {
        if ((bytes[1] & 0xfeu) != 0) {
            return false;
        }
        result.bulk_in_fifo_bytes = (bytes[1] & 1u) != 0;
        if (bytes[0] == kStatusSuccess && result.bulk_in_fifo_bytes) {
            return false;
        }
    }
    return true;
}

bool decode_read_status_byte(std::span<const std::uint8_t> bytes,
                             std::uint8_t expected_tag,
                             bool interrupt_endpoint, std::uint8_t& status,
                             std::uint8_t& status_byte) {
    if (bytes.size() != 3) {
        return false;
    }
    status = bytes[0];
    status_byte = 0;
    if (status != kStatusSuccess) {
        return true;
    }
    if (bytes[1] != expected_tag || (interrupt_endpoint && bytes[2] != 0)) {
        return false;
    }
    if (!interrupt_endpoint) {
        status_byte = bytes[2];
    }
    return true;
}

bool decode_interrupt_status_byte(std::span<const std::uint8_t> bytes,
                                  std::uint8_t& tag,
                                  std::uint8_t& status_byte) {
    if (bytes.size() != 2 || (bytes[0] & 0x80u) == 0) {
        return false;
    }
    tag = bytes[0] & 0x7fu;
    if (tag == 0) {
        return false;
    }
    status_byte = bytes[1];
    return true;
}

}  // namespace wrvisa::usbtmc
