#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "backends/usb/usbtmc_protocol.h"
#include "backends/usb/usbtmc_session.h"
#include "backends/usb/usb_transport.h"
#include "test_support.h"

namespace {

class ScriptedUsbTransport final : public wrvisa::UsbTransport {
public:
    wrvisa::UsbInterfaceInfo interface_info() const noexcept override {
        return info;
    }

    ViStatus bulk_out(wrvisa::Operation& operation,
                      std::span<const std::uint8_t> data,
                      std::size_t& transferred) override {
        if (operation.completed()) {
            transferred = 0;
            return operation.result();
        }
        outgoing.emplace_back(data.begin(), data.end());
        transferred = data.size();
        if (out_statuses.empty()) {
            return VI_SUCCESS;
        }
        const auto status = out_statuses.front();
        out_statuses.pop_front();
        return status;
    }

    ViStatus bulk_in(wrvisa::Operation& operation, std::size_t maximum_size,
                     std::vector<std::uint8_t>& data) override {
        if (operation.completed()) {
            return operation.result();
        }
        if (incoming.empty()) {
            return VI_ERROR_IO;
        }
        auto status = VI_SUCCESS;
        if (!in_statuses.empty()) {
            status = in_statuses.front();
            in_statuses.pop_front();
        }
        data = std::move(incoming.front());
        incoming.pop_front();
        if (enforce_maximum && data.size() > maximum_size) {
            return VI_ERROR_IO;
        }
        return status;
    }

    ViStatus control_in(wrvisa::Operation& operation,
                        const wrvisa::UsbControlRequest& request,
                        std::size_t response_size,
                        std::vector<std::uint8_t>& response) override {
        if (operation.completed()) {
            return operation.result();
        }
        controls.push_back(request);
        if (control_incoming.empty()) {
            return VI_ERROR_IO;
        }
        response = std::move(control_incoming.front());
        control_incoming.pop_front();
        if (response.size() != response_size) {
            return VI_ERROR_IO;
        }
        return VI_SUCCESS;
    }

    ViStatus interrupt_in(wrvisa::Operation& operation,
                          std::size_t maximum_size,
                          std::vector<std::uint8_t>& data) override {
        if (operation.completed()) {
            return operation.result();
        }
        if (interrupt_incoming.empty()) {
            return VI_ERROR_IO;
        }
        data = std::move(interrupt_incoming.front());
        interrupt_incoming.pop_front();
        return data.size() <= maximum_size ? VI_SUCCESS : VI_ERROR_IO;
    }

    ViStatus clear_halt(wrvisa::Operation& operation,
                        std::uint8_t endpoint_address) override {
        if (operation.completed()) {
            return operation.result();
        }
        cleared_halts.push_back(endpoint_address);
        return VI_SUCCESS;
    }

    void cancel() noexcept override { ++cancel_count; }
    void close() noexcept override { closed = true; }

    std::vector<std::vector<std::uint8_t>> outgoing;
    std::deque<std::vector<std::uint8_t>> incoming;
    std::deque<ViStatus> out_statuses;
    std::deque<ViStatus> in_statuses;
    std::vector<wrvisa::UsbControlRequest> controls;
    std::deque<std::vector<std::uint8_t>> control_incoming;
    std::deque<std::vector<std::uint8_t>> interrupt_incoming;
    std::vector<std::uint8_t> cleared_halts;
    wrvisa::UsbInterfaceInfo info{0, 0x02, 0x81, 0x83, 64, true};
    std::size_t cancel_count{0};
    bool enforce_maximum{true};
    bool closed{false};
};

std::span<const std::uint8_t> bytes_of(const std::string& value) {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

std::vector<std::uint8_t> usb488_capabilities(bool interrupt = true,
                                               bool trigger = true) {
    std::vector<std::uint8_t> response(24, 0);
    response[0] = wrvisa::usbtmc::kStatusSuccess;
    response[3] = 1;
    response[5] = 1;
    response[13] = 1;
    response[14] = trigger ? 1u : 0u;
    response[15] = trigger ? 1u : 0u;
    if (interrupt) {
        response[14] |= 4u;
        response[15] |= 4u;
    }
    return response;
}

}  // namespace

int main() {
    using namespace wrvisa;

    const std::string abc = "abc";
    const auto encoded_out =
        usbtmc::encode_dev_dep_msg_out(1, bytes_of(abc), true);
    CHECK(encoded_out.has_value());
    const std::array<std::uint8_t, 16> expected_out{{
        1, 1, 0xFE, 0, 3, 0, 0, 0, 1, 0, 0, 0, 'a', 'b', 'c', 0,
    }};
    CHECK(*encoded_out ==
          std::vector<std::uint8_t>(expected_out.begin(), expected_out.end()));

    usbtmc::DevDepMessage message;
    CHECK(usbtmc::decode_dev_dep_msg_out(*encoded_out, message));
    CHECK(message.tag == 1);
    CHECK(message.end_of_message);
    CHECK(std::string(message.payload.begin(), message.payload.end()) == "abc");

    const auto request =
        usbtmc::encode_request_dev_dep_msg_in(2, 1024, true, '\n');
    CHECK(request.has_value());
    const std::array<std::uint8_t, 12> expected_request{{
        2, 2, 0xFD, 0, 0, 4, 0, 0, 2, '\n', 0, 0,
    }};
    CHECK(*request == std::vector<std::uint8_t>(expected_request.begin(),
                                                expected_request.end()));
    std::uint8_t request_tag = 0;
    std::uint32_t request_size = 0;
    bool termchar_enabled = false;
    std::uint8_t termchar = 0;
    CHECK(usbtmc::decode_request_dev_dep_msg_in(
        *request, request_tag, request_size, termchar_enabled, termchar));
    CHECK(request_tag == 2);
    CHECK(request_size == 1024);
    CHECK(termchar_enabled);
    CHECK(termchar == '\n');

    const auto encoded_in =
        usbtmc::encode_dev_dep_msg_in(2, bytes_of(abc), true);
    CHECK(encoded_in.has_value());
    CHECK(usbtmc::decode_dev_dep_msg_in(*encoded_in, 2, message));
    CHECK(!usbtmc::decode_dev_dep_msg_in(*encoded_in, 3, message));
    auto malformed = *encoded_in;
    malformed[2] = 0;
    CHECK(!usbtmc::decode_dev_dep_msg_in(malformed, 2, message));
    malformed = *encoded_in;
    malformed.back() = 1;
    CHECK(usbtmc::decode_dev_dep_msg_in(malformed, 2, message));
    malformed.resize(malformed.size() - 2u);
    CHECK(!usbtmc::decode_dev_dep_msg_in(malformed, 2, message));
    malformed = *encoded_in;
    malformed.resize(usbtmc::kHeaderSize + abc.size());
    malformed.insert(malformed.end(), 63, 0xA5);
    CHECK(usbtmc::decode_dev_dep_msg_in(malformed, 2, message,
                                        usbtmc::kMaximumPayload, 63));
    malformed.push_back(0);
    CHECK(!usbtmc::decode_dev_dep_msg_in(malformed, 2, message,
                                         usbtmc::kMaximumPayload, 63));
    CHECK(!usbtmc::encode_dev_dep_msg_out(0, bytes_of(abc), true));
    CHECK(!usbtmc::encode_request_dev_dep_msg_in(1, 0, false, 0));

    const auto trigger_frame = usbtmc::encode_usb488_trigger(7);
    CHECK(trigger_frame.has_value());
    const std::array<std::uint8_t, 12> expected_trigger{{
        128, 7, 0xF8, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    }};
    CHECK(*trigger_frame == std::vector<std::uint8_t>(
                                expected_trigger.begin(),
                                expected_trigger.end()));
    std::uint8_t decoded_tag = 0;
    CHECK(usbtmc::decode_usb488_trigger(*trigger_frame, decoded_tag));
    CHECK(decoded_tag == 7);
    malformed = *trigger_frame;
    malformed[4] = 1;
    CHECK(!usbtmc::decode_usb488_trigger(malformed, decoded_tag));
    CHECK(!usbtmc::encode_usb488_trigger(0));

    usbtmc::Capabilities capabilities;
    const auto capabilities_response = usb488_capabilities();
    CHECK(usbtmc::decode_capabilities(capabilities_response, true,
                                      capabilities));
    CHECK(capabilities.status == usbtmc::kStatusSuccess);
    CHECK(capabilities.termchar);
    CHECK(capabilities.usb488);
    CHECK(capabilities.usb488_trigger);
    CHECK(capabilities.usb488_service_request);
    auto malformed_capabilities = capabilities_response;
    malformed_capabilities[16] = 1;
    CHECK(!usbtmc::decode_capabilities(malformed_capabilities, true,
                                       capabilities));
    malformed_capabilities = capabilities_response;
    malformed_capabilities[14] &= static_cast<std::uint8_t>(~1u);
    CHECK(!usbtmc::decode_capabilities(malformed_capabilities, true,
                                       capabilities));

    std::uint8_t device_status = 0;
    CHECK(usbtmc::decode_initiate_abort(
        std::vector<std::uint8_t>{usbtmc::kStatusSuccess, 9}, 9,
        device_status));
    CHECK(device_status == usbtmc::kStatusSuccess);
    CHECK(!usbtmc::decode_initiate_abort(
        std::vector<std::uint8_t>{usbtmc::kStatusSuccess, 8}, 9,
        device_status));
    usbtmc::SplitStatus split_status;
    CHECK(usbtmc::decode_check_abort_bulk_out(
        std::vector<std::uint8_t>{usbtmc::kStatusPending, 0, 0, 0,
                                  0, 0, 0, 0},
        split_status));
    CHECK(split_status.status == usbtmc::kStatusPending);
    CHECK(usbtmc::decode_check_abort_bulk_in(
        std::vector<std::uint8_t>{usbtmc::kStatusPending, 1, 0, 0,
                                  0, 0, 0, 0},
        split_status));
    CHECK(split_status.bulk_in_fifo_bytes);
    CHECK(!usbtmc::decode_check_abort_bulk_in(
        std::vector<std::uint8_t>{usbtmc::kStatusSuccess, 1, 0, 0,
                                  0, 0, 0, 0},
        split_status));
    CHECK(usbtmc::decode_check_clear(
        std::vector<std::uint8_t>{usbtmc::kStatusPending, 1},
        split_status));
    CHECK(!usbtmc::decode_check_clear(
        std::vector<std::uint8_t>{usbtmc::kStatusSuccess, 1},
        split_status));
    std::uint8_t decoded_status_byte = 0;
    CHECK(usbtmc::decode_read_status_byte(
        std::vector<std::uint8_t>{usbtmc::kStatusSuccess, 2, 0},
        2, true, device_status, decoded_status_byte));
    CHECK(usbtmc::decode_interrupt_status_byte(
        std::vector<std::uint8_t>{0x82, 0x42}, decoded_tag,
        decoded_status_byte));
    CHECK(decoded_tag == 2);
    CHECK(decoded_status_byte == 0x42);

    auto transport = std::make_unique<ScriptedUsbTransport>();
    auto* scripted = transport.get();
    UsbTmcBackendSession session(std::move(transport));

    const std::string query = "*IDN?\n";
    Operation write_operation(1000);
    ViUInt32 written = 0;
    CHECK(session.write(write_operation,
                        reinterpret_cast<ViConstBuf>(query.data()),
                        static_cast<ViUInt32>(query.size()), &written,
                        WriteOptions{}) == VI_SUCCESS);
    CHECK(written == query.size());
    CHECK(scripted->outgoing.size() == 1);
    CHECK(usbtmc::decode_dev_dep_msg_out(scripted->outgoing[0], message));
    CHECK(std::string(message.payload.begin(), message.payload.end()) == query);

    const std::string reply = "VALUE\nTAIL";
    auto reply_frame = usbtmc::encode_dev_dep_msg_in(2, bytes_of(reply), true);
    CHECK(reply_frame.has_value());
    scripted->incoming.push_back(std::move(*reply_frame));
    std::array<ViByte, 32> buffer{};
    ViUInt32 received = 0;
    Operation read_operation(1000);
    CHECK(session.read(read_operation, buffer.data(), 32, &received,
                       ReadOptions{true, '\n'}) == VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<const char*>(buffer.data()), received) ==
          "VALUE\n");
    CHECK(scripted->outgoing.size() == 2);
    CHECK(usbtmc::decode_request_dev_dep_msg_in(
        scripted->outgoing[1], request_tag, request_size, termchar_enabled,
        termchar));
    CHECK(request_tag == 2);
    CHECK(request_size == 32);

    Operation tail_operation(1000);
    CHECK(session.read(tail_operation, buffer.data(), 32, &received,
                       ReadOptions{}) == VI_SUCCESS);
    CHECK(std::string(reinterpret_cast<const char*>(buffer.data()), received) ==
          "TAIL");
    CHECK(scripted->outgoing.size() == 2);

    scripted->control_incoming.push_back(usb488_capabilities());
    scripted->control_incoming.push_back(
        {usbtmc::kStatusSuccess, 2, 0});
    scripted->interrupt_incoming.push_back({0x81, 0x11});
    scripted->interrupt_incoming.push_back({0x82, 0x42});
    ViUInt16 status_byte = 0;
    Operation stb_operation(1000);
    CHECK(session.read_stb(stb_operation, &status_byte) == VI_SUCCESS);
    CHECK(status_byte == 0x42);
    Operation trigger_operation(1000);
    CHECK(session.assert_trigger(trigger_operation, VI_TRIG_PROT_DEFAULT) ==
          VI_SUCCESS);
    CHECK(scripted->outgoing.size() == 3);
    CHECK(usbtmc::decode_usb488_trigger(scripted->outgoing.back(), decoded_tag));
    CHECK(decoded_tag == 3);

    scripted->control_incoming.push_back({usbtmc::kStatusSuccess});
    scripted->control_incoming.push_back({usbtmc::kStatusPending, 1});
    scripted->incoming.push_back(std::vector<std::uint8_t>(64, 0xA5));
    scripted->incoming.push_back({});
    scripted->control_incoming.push_back({usbtmc::kStatusSuccess, 0});
    Operation clear_operation(1000);
    CHECK(session.clear(clear_operation) == VI_SUCCESS);
    CHECK(scripted->cleared_halts.back() == 0x02);
    CHECK(scripted->controls.size() == 5);
    CHECK(scripted->controls[0].request == 7);
    CHECK(scripted->controls[1].request == 128);
    CHECK(scripted->controls[2].request == 5);
    CHECK(scripted->controls[3].request == 6);
    CHECK(scripted->controls[4].request == 6);
    CHECK(session.flush(0) == VI_ERROR_INV_MASK);
    CHECK(session.flush(VI_READ_BUF_DISCARD) == VI_SUCCESS);
    session.notify_cancel();
    CHECK(scripted->cancel_count == 1);

    Operation cancelled_operation(1000);
    CHECK(cancelled_operation.request_cancel());
    written = 99;
    CHECK(session.write(cancelled_operation,
                        reinterpret_cast<ViConstBuf>(query.data()),
                        static_cast<ViUInt32>(query.size()), &written,
                        WriteOptions{}) == VI_ERROR_ABORT);
    CHECK(written == 0);
    session.close();
    CHECK(scripted->closed);

    auto cancel_read_transport = std::make_unique<ScriptedUsbTransport>();
    auto* cancel_read_scripted = cancel_read_transport.get();
    UsbTmcBackendSession cancel_read_session(std::move(cancel_read_transport));
    cancel_read_scripted->incoming.push_back({});
    cancel_read_scripted->in_statuses.push_back(VI_ERROR_ABORT);
    cancel_read_scripted->incoming.push_back({});
    cancel_read_scripted->control_incoming.push_back(
        {usbtmc::kStatusSuccess, 1});
    cancel_read_scripted->control_incoming.push_back(
        {usbtmc::kStatusSuccess, 0, 0, 0, 0, 0, 0, 0});
    Operation aborted_read(1000);
    CHECK(cancel_read_session.read(aborted_read, buffer.data(), 8, &received,
                                   ReadOptions{}) == VI_ERROR_ABORT);
    CHECK(received == 0);
    CHECK(cancel_read_scripted->controls.size() == 2);
    CHECK(cancel_read_scripted->controls[0].request == 3);
    CHECK(cancel_read_scripted->controls[0].value == 1);
    CHECK(cancel_read_scripted->controls[0].index == 0x81);
    CHECK(cancel_read_scripted->controls[1].request == 4);
    auto after_abort = usbtmc::encode_dev_dep_msg_in(2, bytes_of("OK"), true);
    CHECK(after_abort.has_value());
    cancel_read_scripted->incoming.push_back(std::move(*after_abort));
    Operation read_after_abort(1000);
    CHECK(cancel_read_session.read(read_after_abort, buffer.data(), 8, &received,
                                   ReadOptions{}) == VI_SUCCESS);
    CHECK(std::string(reinterpret_cast<const char*>(buffer.data()), received) ==
          "OK");

    auto cancel_write_transport = std::make_unique<ScriptedUsbTransport>();
    auto* cancel_write_scripted = cancel_write_transport.get();
    UsbTmcBackendSession cancel_write_session(std::move(cancel_write_transport));
    cancel_write_scripted->out_statuses.push_back(VI_ERROR_TMO);
    cancel_write_scripted->control_incoming.push_back(
        {usbtmc::kStatusSuccess, 1});
    cancel_write_scripted->control_incoming.push_back(
        {usbtmc::kStatusPending, 0, 0, 0, 0, 0, 0, 0});
    cancel_write_scripted->control_incoming.push_back(
        {usbtmc::kStatusSuccess, 0, 0, 0, 3, 0, 0, 0});
    Operation timed_write(1000);
    CHECK(cancel_write_session.write(
              timed_write, reinterpret_cast<ViConstBuf>(query.data()),
              static_cast<ViUInt32>(query.size()), &written,
              WriteOptions{}) == VI_ERROR_TMO);
    CHECK(written == 0);
    CHECK(cancel_write_scripted->controls.size() == 3);
    CHECK(cancel_write_scripted->controls[0].request == 1);
    CHECK(cancel_write_scripted->controls[1].request == 2);
    CHECK(cancel_write_scripted->controls[2].request == 2);
    CHECK(cancel_write_scripted->cleared_halts ==
          std::vector<std::uint8_t>({0x02}));
    Operation write_after_timeout(1000);
    CHECK(cancel_write_session.write(
              write_after_timeout,
              reinterpret_cast<ViConstBuf>(query.data()),
              static_cast<ViUInt32>(query.size()), &written,
              WriteOptions{}) == VI_SUCCESS);

    auto fallback_transport = std::make_unique<ScriptedUsbTransport>();
    auto* fallback_scripted = fallback_transport.get();
    UsbTmcBackendSession fallback_session(std::move(fallback_transport));
    fallback_scripted->out_statuses.push_back(VI_ERROR_ABORT);
    fallback_scripted->control_incoming.push_back(
        {usbtmc::kStatusTransferNotInProgress, 1});
    fallback_scripted->control_incoming.push_back(
        {usbtmc::kStatusSuccess});
    fallback_scripted->control_incoming.push_back(
        {usbtmc::kStatusSuccess, 0});
    Operation fallback_write(1000);
    CHECK(fallback_session.write(
              fallback_write, reinterpret_cast<ViConstBuf>(query.data()),
              static_cast<ViUInt32>(query.size()), &written,
              WriteOptions{}) == VI_ERROR_ABORT);
    CHECK(fallback_scripted->controls.size() == 3);
    CHECK(fallback_scripted->controls[0].request == 1);
    CHECK(fallback_scripted->controls[1].request == 5);
    CHECK(fallback_scripted->controls[2].request == 6);
    Operation fallback_reuse(1000);
    CHECK(fallback_session.write(
              fallback_reuse, reinterpret_cast<ViConstBuf>(query.data()),
              static_cast<ViUInt32>(query.size()), &written,
              WriteOptions{}) == VI_SUCCESS);

    auto control_status_transport = std::make_unique<ScriptedUsbTransport>();
    auto* control_status_scripted = control_status_transport.get();
    control_status_scripted->info.interrupt_in_endpoint = 0;
    UsbTmcBackendSession control_status_session(
        std::move(control_status_transport));
    control_status_scripted->control_incoming.push_back(
        usb488_capabilities(false, true));
    control_status_scripted->control_incoming.push_back(
        {usbtmc::kStatusSuccess, 2, 0x55});
    Operation control_status_operation(1000);
    status_byte = 0;
    CHECK(control_status_session.read_stb(control_status_operation,
                                          &status_byte) == VI_SUCCESS);
    CHECK(status_byte == 0x55);

    auto unsupported_trigger_transport =
        std::make_unique<ScriptedUsbTransport>();
    auto* unsupported_trigger_scripted = unsupported_trigger_transport.get();
    UsbTmcBackendSession unsupported_trigger_session(
        std::move(unsupported_trigger_transport));
    unsupported_trigger_scripted->control_incoming.push_back(
        usb488_capabilities(true, false));
    Operation unsupported_trigger_operation(1000);
    CHECK(unsupported_trigger_session.assert_trigger(
              unsupported_trigger_operation, VI_TRIG_PROT_DEFAULT) ==
          VI_ERROR_NSUP_OPER);
    CHECK(unsupported_trigger_scripted->outgoing.empty());

    auto busy_status_transport = std::make_unique<ScriptedUsbTransport>();
    auto* busy_status_scripted = busy_status_transport.get();
    UsbTmcBackendSession busy_status_session(std::move(busy_status_transport));
    busy_status_scripted->control_incoming.push_back(usb488_capabilities());
    busy_status_scripted->control_incoming.push_back(
        {usbtmc::kStatusInterruptInBusy, 2, 0});
    Operation busy_status_operation(1000);
    CHECK(busy_status_session.read_stb(busy_status_operation, &status_byte) ==
          VI_ERROR_RSRC_BUSY);

    auto wrong_interrupt_transport = std::make_unique<ScriptedUsbTransport>();
    auto* wrong_interrupt_scripted = wrong_interrupt_transport.get();
    UsbTmcBackendSession wrong_interrupt_session(
        std::move(wrong_interrupt_transport));
    wrong_interrupt_scripted->control_incoming.push_back(
        usb488_capabilities());
    wrong_interrupt_scripted->control_incoming.push_back(
        {usbtmc::kStatusSuccess, 2, 0});
    wrong_interrupt_scripted->interrupt_incoming.push_back({0x83, 0x44});
    wrong_interrupt_scripted->interrupt_incoming.push_back({0x82, 0x45});
    Operation wrong_interrupt_operation(1000);
    CHECK(wrong_interrupt_session.read_stb(wrong_interrupt_operation,
                                           &status_byte) == VI_SUCCESS);
    CHECK(status_byte == 0x45);

    auto broken_clear_transport = std::make_unique<ScriptedUsbTransport>();
    auto* broken_clear_scripted = broken_clear_transport.get();
    UsbTmcBackendSession broken_clear_session(std::move(broken_clear_transport));
    broken_clear_scripted->control_incoming.push_back(
        {usbtmc::kStatusSuccess});
    broken_clear_scripted->control_incoming.push_back(
        {usbtmc::kStatusSuccess, 1});
    Operation broken_clear_operation(1000);
    CHECK(broken_clear_session.clear(broken_clear_operation) == VI_ERROR_IO);
    CHECK(broken_clear_scripted->closed);
    Operation after_broken_clear(1000);
    CHECK(broken_clear_session.write(
              after_broken_clear,
              reinterpret_cast<ViConstBuf>(query.data()),
              static_cast<ViUInt32>(query.size()), &written,
              WriteOptions{}) == VI_ERROR_CONN_LOST);

    auto failed_recovery_transport = std::make_unique<ScriptedUsbTransport>();
    auto* failed_recovery_scripted = failed_recovery_transport.get();
    UsbTmcBackendSession failed_recovery_session(
        std::move(failed_recovery_transport));
    failed_recovery_scripted->out_statuses.push_back(VI_ERROR_ABORT);
    Operation failed_recovery_operation(1000);
    CHECK(failed_recovery_session.write(
              failed_recovery_operation,
              reinterpret_cast<ViConstBuf>(query.data()),
              static_cast<ViUInt32>(query.size()), &written,
              WriteOptions{}) == VI_ERROR_ABORT);
    CHECK(failed_recovery_scripted->closed);
    CHECK(failed_recovery_scripted->controls.size() == 2);
    CHECK(failed_recovery_scripted->controls[0].request == 1);
    CHECK(failed_recovery_scripted->controls[1].request == 5);
    Operation after_failed_recovery(1000);
    CHECK(failed_recovery_session.write(
              after_failed_recovery,
              reinterpret_cast<ViConstBuf>(query.data()),
              static_cast<ViUInt32>(query.size()), &written,
              WriteOptions{}) == VI_ERROR_CONN_LOST);

    auto restore_transport = std::make_unique<ScriptedUsbTransport>();
    auto* restore_scripted = restore_transport.get();
    UsbTmcBackendSession restore_session(std::move(restore_transport));
    auto partial = usbtmc::encode_dev_dep_msg_in(1, bytes_of("AB"), false);
    CHECK(partial.has_value());
    restore_scripted->incoming.push_back(std::move(*partial));
    Operation failed_read(1000);
    CHECK(restore_session.read(failed_read, buffer.data(), 8, &received,
                               ReadOptions{}) == VI_ERROR_IO);
    CHECK(received == 0);
    auto completion = usbtmc::encode_dev_dep_msg_in(3, bytes_of("C"), true);
    CHECK(completion.has_value());
    restore_scripted->incoming.push_back(std::move(*completion));
    Operation recovered_read(1000);
    CHECK(restore_session.read(recovered_read, buffer.data(), 8, &received,
                               ReadOptions{}) == VI_SUCCESS);
    CHECK(std::string(reinterpret_cast<const char*>(buffer.data()), received) ==
          "ABC");

    auto bad_transport = std::make_unique<ScriptedUsbTransport>();
    auto* bad_scripted = bad_transport.get();
    UsbTmcBackendSession bad_session(std::move(bad_transport));
    auto wrong_tag = usbtmc::encode_dev_dep_msg_in(9, bytes_of(abc), true);
    CHECK(wrong_tag.has_value());
    bad_scripted->incoming.push_back(std::move(*wrong_tag));
    Operation bad_operation(1000);
    CHECK(bad_session.read(bad_operation, buffer.data(), 8, &received,
                           ReadOptions{}) == VI_ERROR_IO);
    CHECK(received == 0);

    auto invalid_transport = std::make_unique<ScriptedUsbTransport>();
    auto* invalid_scripted = invalid_transport.get();
    invalid_scripted->info.bulk_in_max_packet_size = 0;
    UsbTmcBackendSession invalid_session(std::move(invalid_transport));
    Operation invalid_read(1000);
    CHECK(invalid_session.read(invalid_read, buffer.data(), 8, &received,
                               ReadOptions{}) == VI_ERROR_NSUP_OPER);
    Operation invalid_write(1000);
    CHECK(invalid_session.write(
              invalid_write, reinterpret_cast<ViConstBuf>(query.data()),
              static_cast<ViUInt32>(query.size()), &written,
              WriteOptions{}) == VI_ERROR_NSUP_OPER);
    CHECK(invalid_scripted->outgoing.empty());
    return 0;
}
