#ifndef WRVISA_BACKENDS_USB_USBTMC_PROTOCOL_H
#define WRVISA_BACKENDS_USB_USBTMC_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wrvisa::usbtmc {

constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kMaximumPayload = 64u * 1024u * 1024u;
constexpr std::uint8_t kDevDepMsgOut = 1;
constexpr std::uint8_t kRequestDevDepMsgIn = 2;
constexpr std::uint8_t kDevDepMsgIn = 2;
constexpr std::uint8_t kUsb488Trigger = 128;

constexpr std::uint8_t kStatusSuccess = 0x01;
constexpr std::uint8_t kStatusPending = 0x02;
constexpr std::uint8_t kStatusInterruptInBusy = 0x20;
constexpr std::uint8_t kStatusFailed = 0x80;
constexpr std::uint8_t kStatusTransferNotInProgress = 0x81;

struct Capabilities {
    std::uint8_t status{0};
    bool termchar{false};
    bool usb488{false};
    bool usb488_trigger{false};
    bool usb488_service_request{false};
};

struct SplitStatus {
    std::uint8_t status{0};
    bool bulk_in_fifo_bytes{false};
    std::uint32_t transferred{0};
};

struct DevDepMessage {
    std::uint8_t tag{0};
    bool end_of_message{false};
    std::vector<std::uint8_t> payload;
};

std::optional<std::vector<std::uint8_t>> encode_dev_dep_msg_out(
    std::uint8_t tag, std::span<const std::uint8_t> payload,
    bool end_of_message);
std::optional<std::vector<std::uint8_t>> encode_request_dev_dep_msg_in(
    std::uint8_t tag, std::uint32_t transfer_size, bool termchar_enabled,
    std::uint8_t termchar);
std::optional<std::vector<std::uint8_t>> encode_dev_dep_msg_in(
    std::uint8_t tag, std::span<const std::uint8_t> payload,
    bool end_of_message);
std::optional<std::vector<std::uint8_t>> encode_usb488_trigger(
    std::uint8_t tag);
bool decode_dev_dep_msg_out(std::span<const std::uint8_t> bytes,
                            DevDepMessage& message,
                            std::size_t maximum_payload = kMaximumPayload);
bool decode_request_dev_dep_msg_in(std::span<const std::uint8_t> bytes,
                                   std::uint8_t& tag,
                                   std::uint32_t& transfer_size,
                                   bool& termchar_enabled,
                                   std::uint8_t& termchar);
bool decode_dev_dep_msg_in(std::span<const std::uint8_t> bytes,
                           std::uint8_t expected_tag, DevDepMessage& message,
                           std::size_t maximum_payload = kMaximumPayload,
                           std::size_t maximum_alignment = 3);
bool decode_usb488_trigger(std::span<const std::uint8_t> bytes,
                           std::uint8_t& tag);
bool decode_capabilities(std::span<const std::uint8_t> bytes,
                         bool expect_usb488, Capabilities& capabilities);
bool decode_initiate_abort(std::span<const std::uint8_t> bytes,
                           std::uint8_t expected_tag, std::uint8_t& status);
bool decode_check_abort_bulk_out(std::span<const std::uint8_t> bytes,
                                 SplitStatus& result);
bool decode_check_abort_bulk_in(std::span<const std::uint8_t> bytes,
                                SplitStatus& result);
bool decode_initiate_clear(std::span<const std::uint8_t> bytes,
                           std::uint8_t& status);
bool decode_check_clear(std::span<const std::uint8_t> bytes,
                        SplitStatus& result);
bool decode_read_status_byte(std::span<const std::uint8_t> bytes,
                             std::uint8_t expected_tag,
                             bool interrupt_endpoint, std::uint8_t& status,
                             std::uint8_t& status_byte);
bool decode_interrupt_status_byte(std::span<const std::uint8_t> bytes,
                                  std::uint8_t& tag,
                                  std::uint8_t& status_byte);

}  // namespace wrvisa::usbtmc

#endif
