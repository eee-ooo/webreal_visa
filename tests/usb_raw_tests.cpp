#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "backends/usb/usb_raw_session.h"
#include "test_support.h"

namespace {

class RawTransport final : public wrvisa::UsbTransport {
public:
    wrvisa::UsbInterfaceInfo interface_info() const noexcept override {
        return {2, 0x02, 0x81, 0x83, 64, false, 1, 0x04};
    }

    ViStatus bulk_out(wrvisa::Operation& operation,
                      std::span<const std::uint8_t> data,
                      std::size_t& transferred) override {
        if (operation.completed()) {
            transferred = 0;
            return operation.result();
        }
        bulk_written.assign(data.begin(), data.end());
        transferred = short_write ? data.size() - 1u : data.size();
        return VI_SUCCESS;
    }

    ViStatus bulk_in(wrvisa::Operation& operation, std::size_t maximum_size,
                     std::vector<std::uint8_t>& data) override {
        if (operation.completed()) {
            return operation.result();
        }
        CHECK(bulk_read.size() <= maximum_size);
        data = bulk_read;
        bulk_read.clear();
        return VI_SUCCESS;
    }

    ViStatus control_in(wrvisa::Operation& operation,
                        const wrvisa::UsbControlRequest& request,
                        std::size_t response_size,
                        std::vector<std::uint8_t>& response) override {
        if (operation.completed()) {
            return operation.result();
        }
        last_control = request;
        response.assign(response_size, request.request);
        return VI_SUCCESS;
    }

    ViStatus control_out(wrvisa::Operation& operation,
                         const wrvisa::UsbControlRequest& request,
                         std::span<const std::uint8_t> data,
                         std::size_t& transferred) override {
        if (operation.completed()) {
            transferred = 0;
            return operation.result();
        }
        last_control = request;
        control_written.assign(data.begin(), data.end());
        transferred = data.size();
        return VI_SUCCESS;
    }

    ViStatus interrupt_in(wrvisa::Operation& operation,
                          std::size_t maximum_size,
                          std::vector<std::uint8_t>& data) override {
        if (operation.completed()) {
            return operation.result();
        }
        CHECK(interrupt_read.size() <= maximum_size);
        data = interrupt_read;
        interrupt_read.clear();
        return VI_SUCCESS;
    }

    ViStatus interrupt_out(wrvisa::Operation& operation,
                           std::span<const std::uint8_t> data,
                           std::size_t& transferred) override {
        if (operation.completed()) {
            transferred = 0;
            return operation.result();
        }
        interrupt_written.assign(data.begin(), data.end());
        transferred = data.size();
        return VI_SUCCESS;
    }

    ViStatus clear_halt(wrvisa::Operation& operation,
                        std::uint8_t endpoint) override {
        if (operation.completed()) {
            return operation.result();
        }
        cleared.push_back(endpoint);
        return VI_SUCCESS;
    }

    void cancel() noexcept override { ++cancels; }
    void close() noexcept override { ++closes; }

    std::vector<std::uint8_t> bulk_read;
    std::vector<std::uint8_t> interrupt_read;
    std::vector<std::uint8_t> bulk_written;
    std::vector<std::uint8_t> interrupt_written;
    std::vector<std::uint8_t> control_written;
    std::vector<std::uint8_t> cleared;
    wrvisa::UsbControlRequest last_control{};
    std::size_t cancels{0};
    std::size_t closes{0};
    bool short_write{false};
};

}  // namespace

int main() {
    using wrvisa::Operation;
    using wrvisa::UsbRawBackendSession;
    using wrvisa::UsbRawConfiguration;
    using wrvisa::UsbTransferType;

    auto transport = std::make_unique<RawTransport>();
    auto* raw_transport = transport.get();
    UsbRawBackendSession bulk_session(
        std::move(transport),
        UsbRawConfiguration{1, UsbTransferType::bulk, 0x81,
                            UsbTransferType::bulk, 0x02});

    constexpr std::array<ViByte, 3> output{'A', 'B', 'C'};
    ViUInt32 count = 0;
    Operation write_operation(1000);
    CHECK(bulk_session.write(write_operation, output.data(), output.size(),
                             &count, {}) == VI_SUCCESS);
    CHECK(count == output.size());
    CHECK(raw_transport->bulk_written ==
          std::vector<std::uint8_t>(output.begin(), output.end()));
    raw_transport->short_write = true;
    count = 99;
    Operation short_write_operation(1000);
    CHECK(bulk_session.write(short_write_operation, output.data(), output.size(),
                             &count, {}) == VI_ERROR_IO);
    CHECK(count == 0);
    raw_transport->short_write = false;

    raw_transport->bulk_read = {'A', '\n', 'B'};
    std::array<ViByte, 8> input{};
    Operation read_operation(1000);
    CHECK(bulk_session.read(read_operation, input.data(), input.size(), &count,
                            {true, '\n'}) == VI_SUCCESS_TERM_CHAR);
    CHECK(count == 2);
    CHECK(std::string(reinterpret_cast<const char*>(input.data()), count) ==
          "A\n");
    Operation read_ahead_operation(1000);
    CHECK(bulk_session.read(read_ahead_operation, input.data(), input.size(),
                            &count, {}) == VI_SUCCESS);
    CHECK(count == 1 && input[0] == 'B');

    std::array<ViByte, 4> control_data{};
    Operation control_in_operation(1000);
    CHECK(bulk_session.usb_control(control_in_operation, 0xC0, 0x55, 7, 2,
                                   control_data.data(), control_data.size(),
                                   &count) == VI_SUCCESS);
    CHECK(count == control_data.size());
    CHECK(control_data[0] == 0x55 && raw_transport->last_control.value == 7);
    control_data = {1, 2, 3, 4};
    Operation control_out_operation(1000);
    CHECK(bulk_session.usb_control(control_out_operation, 0x40, 0x56, 8, 3,
                                   control_data.data(), control_data.size(),
                                   &count) == VI_SUCCESS);
    CHECK(raw_transport->control_written ==
          std::vector<std::uint8_t>(control_data.begin(), control_data.end()));

    Operation clear_operation(1000);
    CHECK(bulk_session.clear(clear_operation) == VI_SUCCESS);
    CHECK(raw_transport->cleared == std::vector<std::uint8_t>({0x81, 0x02}));
    CHECK(bulk_session.flush(0) == VI_ERROR_INV_MASK);
    CHECK(bulk_session.flush(VI_READ_BUF_DISCARD) == VI_SUCCESS);
    CHECK(bulk_session.read_stb(nullptr) == VI_ERROR_NSUP_OPER);
    bulk_session.notify_cancel();
    CHECK(raw_transport->cancels == 1);
    bulk_session.close();
    CHECK(raw_transport->closes == 1);

    auto interrupt_transport = std::make_unique<RawTransport>();
    auto* raw_interrupt_transport = interrupt_transport.get();
    raw_interrupt_transport->interrupt_read = {9, 8};
    UsbRawBackendSession interrupt_session(
        std::move(interrupt_transport),
        UsbRawConfiguration{1, UsbTransferType::interrupt, 0x83,
                            UsbTransferType::interrupt, 0x04});
    Operation interrupt_read_operation(1000);
    CHECK(interrupt_session.read(interrupt_read_operation, input.data(), 2,
                                 &count, {}) == VI_SUCCESS_MAX_CNT);
    CHECK(count == 2 && input[0] == 9 && input[1] == 8);
    Operation interrupt_write_operation(1000);
    CHECK(interrupt_session.write(interrupt_write_operation, output.data(),
                                  output.size(), &count, {}) == VI_SUCCESS);
    CHECK(raw_interrupt_transport->interrupt_written ==
          std::vector<std::uint8_t>(output.begin(), output.end()));

    auto invalid_transport = std::make_unique<RawTransport>();
    UsbRawBackendSession invalid_session(
        std::move(invalid_transport),
        UsbRawConfiguration{0, UsbTransferType::bulk, 0x81,
                            UsbTransferType::bulk, 0x02});
    Operation invalid_operation(1000);
    CHECK(invalid_session.read(invalid_operation, input.data(), input.size(),
                               &count, {}) == VI_ERROR_INV_PARAMETER);

    auto none_transport = std::make_unique<RawTransport>();
    UsbRawBackendSession none_session(
        std::move(none_transport),
        UsbRawConfiguration{1, UsbTransferType::none, 0,
                            UsbTransferType::none, 0});
    Operation none_operation(1000);
    CHECK(none_session.read(none_operation, input.data(), input.size(), &count,
                            {}) == VI_ERROR_NSUP_OPER);
    return 0;
}
