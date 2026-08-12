#ifndef WRVISA_BACKENDS_HISLIP_HISLIP_PROTOCOL_H
#define WRVISA_BACKENDS_HISLIP_HISLIP_PROTOCOL_H

#include <cstdint>
#include <span>
#include <vector>

namespace wrvisa::hislip {

enum class MessageType : std::uint8_t {
    initialize = 0,
    initialize_response = 1,
    fatal_error = 2,
    error = 3,
    async_lock = 4,
    async_lock_response = 5,
    data = 6,
    data_end = 7,
    device_clear_complete = 8,
    device_clear_acknowledge = 9,
    async_remote_local_control = 10,
    async_remote_local_response = 11,
    trigger = 12,
    interrupted = 13,
    async_interrupted = 14,
    async_maximum_message_size = 15,
    async_maximum_message_size_response = 16,
    async_initialize = 17,
    async_initialize_response = 18,
    async_device_clear = 19,
    async_service_request = 20,
    async_status_query = 21,
    async_status_response = 22,
    async_device_clear_acknowledge = 23,
    async_lock_info = 24,
    async_lock_info_response = 25,
};

struct Frame {
    MessageType type{MessageType::fatal_error};
    std::uint8_t control{0};
    std::uint32_t parameter{0};
    std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> encode(MessageType type, std::uint8_t control,
                                 std::uint32_t parameter,
                                 std::span<const std::uint8_t> payload = {});
bool decode(std::span<const std::uint8_t> bytes, Frame& frame);

}  // namespace wrvisa::hislip

#endif
