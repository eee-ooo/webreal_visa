#ifndef WRVISA_BACKENDS_VXI11_VXI11_PROTOCOL_H
#define WRVISA_BACKENDS_VXI11_VXI11_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace wrvisa::vxi11 {

constexpr std::uint32_t kPortmapperProgram = 100000;
constexpr std::uint32_t kPortmapperVersion = 2;
constexpr std::uint32_t kPortmapperGetPort = 3;
constexpr std::uint32_t kTcpProtocol = 6;
constexpr std::uint32_t kDeviceCoreProgram = UINT32_C(0x0607AF);
constexpr std::uint32_t kDeviceAsyncProgram = UINT32_C(0x0607B0);
constexpr std::uint32_t kProgramVersion = 1;

constexpr std::uint32_t kCreateLink = 10;
constexpr std::uint32_t kDeviceWrite = 11;
constexpr std::uint32_t kDeviceRead = 12;
constexpr std::uint32_t kDeviceReadStb = 13;
constexpr std::uint32_t kDeviceTrigger = 14;
constexpr std::uint32_t kDeviceClear = 15;
constexpr std::uint32_t kDeviceLock = 18;
constexpr std::uint32_t kDeviceUnlock = 19;
constexpr std::uint32_t kDestroyLink = 23;
constexpr std::uint32_t kDeviceAbort = 1;

constexpr std::uint32_t kFlagWaitLock = 1;
constexpr std::uint32_t kFlagEnd = 8;
constexpr std::uint32_t kFlagTermCharSet = 128;
constexpr std::uint32_t kReasonRequestedCount = 1;
constexpr std::uint32_t kReasonTermChar = 2;
constexpr std::uint32_t kReasonEnd = 4;

class XdrWriter final {
public:
    void u32(std::uint32_t value);
    void boolean(bool value);
    void opaque(std::span<const std::uint8_t> value);
    void string(std::string_view value);
    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    std::vector<std::uint8_t> take() noexcept { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

class XdrReader final {
public:
    explicit XdrReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    bool u32(std::uint32_t& value);
    bool opaque(std::vector<std::uint8_t>& value, std::size_t maximum);
    std::span<const std::uint8_t> remaining() const noexcept {
        return bytes_.subspan(offset_);
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{0};
};

std::vector<std::uint8_t> make_rpc_call(std::uint32_t xid,
                                        std::uint32_t program,
                                        std::uint32_t version,
                                        std::uint32_t procedure,
                                        std::span<const std::uint8_t> arguments);
bool parse_rpc_reply(std::span<const std::uint8_t> message,
                     std::uint32_t expected_xid,
                     std::span<const std::uint8_t>& result);

}  // namespace wrvisa::vxi11

#endif
