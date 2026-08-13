#include <libusb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <future>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "backends/usb/usbtmc_protocol.h"
#include "core/handle_table.h"
#include "core/objects.h"
#include "test_support.h"
#include "visa.h"
#include "webreal_visa_ext.h"

namespace {

constexpr const char* kResource =
    "USB0::0x1209::0x0002::WRVISA-LIBUSB-MOCK::0::INSTR";
constexpr const char* kRawResource =
    "USB0::0x1209::0x0002::WRVISA-LIBUSB-MOCK::0::RAW";

std::mutex mock_mutex;
std::condition_variable mock_condition;
std::deque<libusb_transfer*> submitted;
std::set<libusb_transfer*> canceled;
std::string device_response;
std::vector<std::uint8_t> pending_bulk_in;
std::vector<std::uint8_t> pending_interrupt;
bool connected = true;
bool abort_in_drain = false;
std::size_t claims = 0;
std::size_t releases = 0;
std::size_t closes = 0;
std::size_t triggers = 0;
libusb_hotplug_callback_fn hotplug_callback = nullptr;
void* hotplug_user_data = nullptr;

libusb_context* fake_context() {
    return reinterpret_cast<libusb_context*>(static_cast<std::uintptr_t>(1));
}

libusb_device* fake_device() {
    return reinterpret_cast<libusb_device*>(static_cast<std::uintptr_t>(2));
}

libusb_device_handle* fake_handle() {
    return reinterpret_cast<libusb_device_handle*>(
        static_cast<std::uintptr_t>(3));
}

const std::array<libusb_endpoint_descriptor, 4> endpoints{{
    {0, LIBUSB_DT_ENDPOINT, 0x02, LIBUSB_TRANSFER_TYPE_BULK, 64, 0, 0, 0,
     nullptr, 0},
    {0, LIBUSB_DT_ENDPOINT, 0x81, LIBUSB_TRANSFER_TYPE_BULK, 64, 0, 0, 0,
     nullptr, 0},
    {0, LIBUSB_DT_ENDPOINT, 0x83, LIBUSB_TRANSFER_TYPE_INTERRUPT, 8, 1, 0, 0,
     nullptr, 0},
    {0, LIBUSB_DT_ENDPOINT, 0x04, LIBUSB_TRANSFER_TYPE_INTERRUPT, 8, 1, 0, 0,
     nullptr, 0},
}};

const libusb_interface_descriptor interface_descriptor{
    0,
    LIBUSB_DT_INTERFACE,
    0,
    0,
    static_cast<std::uint8_t>(endpoints.size()),
    LIBUSB_CLASS_APPLICATION,
    0x03,
    0x01,
    0,
    endpoints.data(),
    nullptr,
    0,
};

const libusb_interface usb_interface{&interface_descriptor, 1};

const libusb_config_descriptor config_descriptor{
    0,
    LIBUSB_DT_CONFIG,
    0,
    1,
    1,
    0,
    0,
    0,
    &usb_interface,
    nullptr,
    0,
};

bool transfer_ready(libusb_transfer* transfer) {
    if (canceled.contains(transfer) || !connected) {
        return true;
    }
    if (transfer->type != LIBUSB_TRANSFER_TYPE_BULK ||
        transfer->endpoint != 0x81) {
        return true;
    }
    return abort_in_drain || !pending_bulk_in.empty();
}

void process_bulk_out(libusb_transfer* transfer) {
    const auto bytes = std::span<const std::uint8_t>(
        transfer->buffer, static_cast<std::size_t>(transfer->length));
    wrvisa::usbtmc::DevDepMessage message;
    if (wrvisa::usbtmc::decode_dev_dep_msg_out(bytes, message)) {
        const std::string command(message.payload.begin(), message.payload.end());
        device_response = command == "*IDN?\n"
                              ? "WEBREAL,LIBUSB-MOCK,0002,0.5\n"
                              : std::string{};
    } else {
        std::uint8_t tag = 0;
        std::uint32_t requested = 0;
        bool termchar_enabled = false;
        std::uint8_t termchar = 0;
        if (wrvisa::usbtmc::decode_request_dev_dep_msg_in(
                bytes, tag, requested, termchar_enabled, termchar)) {
            static_cast<void>(termchar_enabled);
            static_cast<void>(termchar);
            // A real device may keep the bulk-IN request pending while it has
            // no response data. Model that state explicitly so cancellation
            // and unplug tests do not race an immediate empty EOM response.
            if (device_response.empty()) {
                transfer->status = LIBUSB_TRANSFER_COMPLETED;
                transfer->actual_length = transfer->length;
                return;
            }
            const auto amount =
                std::min<std::size_t>(requested, device_response.size());
            const auto payload = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(device_response.data()),
                amount);
            auto frame = wrvisa::usbtmc::encode_dev_dep_msg_in(
                tag, payload, amount == device_response.size());
            CHECK(frame.has_value());
            pending_bulk_in = std::move(*frame);
            device_response.erase(0, amount);
        } else if (wrvisa::usbtmc::decode_usb488_trigger(bytes, tag)) {
            ++triggers;
        } else {
            pending_bulk_in.assign(bytes.begin(), bytes.end());
        }
    }
    transfer->status = LIBUSB_TRANSFER_COMPLETED;
    transfer->actual_length = transfer->length;
}

void process_control(libusb_transfer* transfer) {
    const auto* setup = libusb_control_transfer_get_setup(transfer);
    auto* response = libusb_control_transfer_get_data(transfer);
    std::vector<std::uint8_t> value;
    switch (setup->bRequest) {
        case 1:
        case 3:
            value = {wrvisa::usbtmc::kStatusSuccess,
                     static_cast<std::uint8_t>(
                         libusb_le16_to_cpu(setup->wValue))};
            if (setup->bRequest == 3) {
                abort_in_drain = true;
            }
            break;
        case 2:
        case 4:
            value = {wrvisa::usbtmc::kStatusSuccess, 0, 0, 0, 0, 0, 0, 0};
            break;
        case 5:
            device_response.clear();
            pending_bulk_in.clear();
            value = {wrvisa::usbtmc::kStatusSuccess};
            break;
        case 6:
            value = {wrvisa::usbtmc::kStatusSuccess, 0};
            break;
        case 7:
            value.assign(24, 0);
            value[0] = wrvisa::usbtmc::kStatusSuccess;
            value[3] = 1;
            value[5] = 1;
            value[13] = 1;
            value[14] = 5;
            value[15] = 5;
            break;
        case 128: {
            const auto tag = static_cast<std::uint8_t>(
                libusb_le16_to_cpu(setup->wValue));
            value = {wrvisa::usbtmc::kStatusSuccess, tag, 0};
            pending_interrupt = {
                static_cast<std::uint8_t>(0x80u | tag), 0x42};
            break;
        }
        case 0x30:
            value.resize(static_cast<std::size_t>(
                libusb_le16_to_cpu(setup->wLength)));
            for (std::size_t index = 0; index < value.size(); ++index) {
                value[index] = static_cast<std::uint8_t>(index + 1u);
            }
            break;
        case 0x31:
            transfer->actual_length = static_cast<int>(
                libusb_le16_to_cpu(setup->wLength));
            transfer->status = LIBUSB_TRANSFER_COMPLETED;
            return;
        default:
            transfer->status = LIBUSB_TRANSFER_STALL;
            return;
    }
    const auto maximum = static_cast<std::size_t>(
        libusb_le16_to_cpu(setup->wLength));
    CHECK(value.size() <= maximum);
    std::copy(value.begin(), value.end(), response);
    transfer->actual_length = static_cast<int>(value.size());
    transfer->status = LIBUSB_TRANSFER_COMPLETED;
}

void process_transfer(libusb_transfer* transfer) {
    transfer->actual_length = 0;
    if (canceled.erase(transfer) != 0) {
        transfer->status = LIBUSB_TRANSFER_CANCELLED;
        return;
    }
    if (!connected) {
        transfer->status = LIBUSB_TRANSFER_NO_DEVICE;
        return;
    }
    if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        process_control(transfer);
    } else if (transfer->type == LIBUSB_TRANSFER_TYPE_BULK &&
               transfer->endpoint == 0x02) {
        process_bulk_out(transfer);
    } else if (transfer->type == LIBUSB_TRANSFER_TYPE_BULK &&
               transfer->endpoint == 0x81) {
        transfer->status = LIBUSB_TRANSFER_COMPLETED;
        if (abort_in_drain) {
            abort_in_drain = false;
            return;
        }
        CHECK(pending_bulk_in.size() <=
              static_cast<std::size_t>(transfer->length));
        std::copy(pending_bulk_in.begin(), pending_bulk_in.end(),
                  transfer->buffer);
        transfer->actual_length =
            static_cast<int>(pending_bulk_in.size());
        pending_bulk_in.clear();
    } else if (transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT &&
               transfer->endpoint == 0x83) {
        CHECK(pending_interrupt.size() <=
              static_cast<std::size_t>(transfer->length));
        std::copy(pending_interrupt.begin(), pending_interrupt.end(),
                  transfer->buffer);
        transfer->actual_length =
            static_cast<int>(pending_interrupt.size());
        pending_interrupt.clear();
        transfer->status = LIBUSB_TRANSFER_COMPLETED;
    } else if (transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT &&
               transfer->endpoint == 0x04) {
        pending_interrupt.assign(
            transfer->buffer,
            transfer->buffer + static_cast<std::ptrdiff_t>(transfer->length));
        transfer->actual_length = transfer->length;
        transfer->status = LIBUSB_TRANSFER_COMPLETED;
    } else {
        transfer->status = LIBUSB_TRANSFER_ERROR;
    }
}

void query_identity(ViSession session) {
    constexpr std::string_view query = "*IDN?\n";
    ViUInt32 count = 0;
    CHECK(viWrite(session,
                  reinterpret_cast<ViConstBuf>(query.data()),
                  static_cast<ViUInt32>(query.size()), &count) == VI_SUCCESS);
    CHECK(count == query.size());
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR, '\n') == VI_SUCCESS);
    CHECK(viSetAttribute(session, VI_ATTR_TERMCHAR_EN, VI_TRUE) == VI_SUCCESS);
    std::array<ViByte, 128> buffer{};
    count = 0;
    CHECK(viRead(session, buffer.data(), static_cast<ViUInt32>(buffer.size()),
                 &count) == VI_SUCCESS_TERM_CHAR);
    CHECK(std::string(reinterpret_cast<const char*>(buffer.data()), count) ==
          "WEBREAL,LIBUSB-MOCK,0002,0.5\n");
}

std::shared_ptr<wrvisa::SessionObject> session_object(ViSession session) {
    auto object = wrvisa::get_handle<wrvisa::SessionObject>(
        session, wrvisa::ObjectType::session);
    CHECK(object != nullptr);
    return object;
}

void wait_until_active(const std::shared_ptr<wrvisa::SessionObject>& session) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(1);
    while (session->active_operation_count() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(session->active_operation_count() != 0);
}

}  // namespace

int LIBUSB_CALL libusb_init(libusb_context** context) {
    *context = fake_context();
    return LIBUSB_SUCCESS;
}

void LIBUSB_CALL libusb_exit(libusb_context* context) {
    CHECK(context == fake_context());
}

int LIBUSB_CALL libusb_has_capability(std::uint32_t capability) {
    return capability == LIBUSB_CAP_HAS_HOTPLUG ? 1 : 0;
}

int LIBUSB_CALL libusb_hotplug_register_callback(
    libusb_context* context, int events, int flags, int vendor_id,
    int product_id, int device_class, libusb_hotplug_callback_fn callback,
    void* user_data, libusb_hotplug_callback_handle* callback_handle) {
    CHECK(context == fake_context());
    CHECK(events == (LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                     LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT));
    CHECK(flags == LIBUSB_HOTPLUG_NO_FLAGS);
    CHECK(vendor_id == LIBUSB_HOTPLUG_MATCH_ANY);
    CHECK(product_id == LIBUSB_HOTPLUG_MATCH_ANY);
    CHECK(device_class == LIBUSB_HOTPLUG_MATCH_ANY);
    hotplug_callback = callback;
    hotplug_user_data = user_data;
    *callback_handle = 17;
    return LIBUSB_SUCCESS;
}

void LIBUSB_CALL libusb_hotplug_deregister_callback(
    libusb_context* context, libusb_hotplug_callback_handle callback_handle) {
    CHECK(context == fake_context());
    CHECK(callback_handle == 17);
    hotplug_callback = nullptr;
    hotplug_user_data = nullptr;
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context* context,
                                            libusb_device*** list) {
    CHECK(context == fake_context());
    auto** result = new libusb_device*[2];
    result[0] = connected ? fake_device() : nullptr;
    result[1] = nullptr;
    *list = result;
    return connected ? 1 : 0;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device** list,
                                          int unref_devices) {
    static_cast<void>(unref_devices);
    delete[] list;
}

libusb_device* LIBUSB_CALL libusb_ref_device(libusb_device* device) {
    return device;
}

void LIBUSB_CALL libusb_unref_device(libusb_device* device) {
    CHECK(device == fake_device());
}

int LIBUSB_CALL libusb_get_device_descriptor(
    libusb_device* device, libusb_device_descriptor* descriptor) {
    CHECK(device == fake_device());
    *descriptor = {};
    descriptor->idVendor = 0x1209;
    descriptor->idProduct = 0x0002;
    descriptor->iSerialNumber = 1;
    descriptor->bNumConfigurations = 1;
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_get_active_config_descriptor(
    libusb_device* device, libusb_config_descriptor** config) {
    CHECK(device == fake_device());
    *config = const_cast<libusb_config_descriptor*>(&config_descriptor);
    return LIBUSB_SUCCESS;
}

void LIBUSB_CALL libusb_free_config_descriptor(
    libusb_config_descriptor* config) {
    CHECK(config == &config_descriptor);
}

int LIBUSB_CALL libusb_get_device_string(libusb_device* device,
                                          libusb_device_string_type type,
                                          char* data, int length) {
    CHECK(device == fake_device());
    CHECK(type == LIBUSB_DEVICE_STRING_SERIAL_NUMBER);
    constexpr std::string_view serial = "WRVISA-LIBUSB-MOCK";
    CHECK(length > static_cast<int>(serial.size()));
    std::copy(serial.begin(), serial.end(), data);
    data[serial.size()] = '\0';
    return static_cast<int>(serial.size() + 1);
}

int LIBUSB_CALL libusb_open(libusb_device* device,
                            libusb_device_handle** handle) {
    CHECK(device == fake_device());
    *handle = fake_handle();
    return connected ? LIBUSB_SUCCESS : LIBUSB_ERROR_NO_DEVICE;
}

void LIBUSB_CALL libusb_close(libusb_device_handle* handle) {
    CHECK(handle == fake_handle());
    {
        std::lock_guard lock(mock_mutex);
        ++closes;
    }
    mock_condition.notify_all();
}

int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(
    libusb_device_handle* handle, int enable) {
    CHECK(handle == fake_handle());
    CHECK(enable == 1);
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle* handle,
                                        int interface_number) {
    CHECK(handle == fake_handle());
    CHECK(interface_number == 0);
    ++claims;
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle* handle,
                                          int interface_number) {
    CHECK(handle == fake_handle());
    CHECK(interface_number == 0);
    ++releases;
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_set_interface_alt_setting(
    libusb_device_handle* handle, int interface_number,
    int alternate_setting) {
    CHECK(handle == fake_handle());
    CHECK(interface_number == 0);
    CHECK(alternate_setting == 0);
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_clear_halt(libusb_device_handle* handle,
                                   unsigned char endpoint) {
    CHECK(handle == fake_handle());
    CHECK(endpoint == 0x02 || endpoint == 0x81);
    return connected ? LIBUSB_SUCCESS : LIBUSB_ERROR_NO_DEVICE;
}

std::uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device* device) {
    CHECK(device == fake_device());
    return 1;
}

std::uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device* device) {
    CHECK(device == fake_device());
    return 7;
}

int LIBUSB_CALL libusb_get_port_numbers(libusb_device* device,
                                         std::uint8_t* ports, int length) {
    CHECK(device == fake_device());
    CHECK(length >= 2);
    ports[0] = 2;
    ports[1] = 4;
    return 2;
}

libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int iso_packets) {
    CHECK(iso_packets == 0);
    return static_cast<libusb_transfer*>(
        std::calloc(1, sizeof(libusb_transfer)));
}

void LIBUSB_CALL libusb_free_transfer(libusb_transfer* transfer) {
    std::free(transfer);
}

int LIBUSB_CALL libusb_submit_transfer(libusb_transfer* transfer) {
    {
        std::lock_guard lock(mock_mutex);
        submitted.push_back(transfer);
    }
    mock_condition.notify_all();
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_cancel_transfer(libusb_transfer* transfer) {
    {
        std::lock_guard lock(mock_mutex);
        const auto found = std::find(submitted.begin(), submitted.end(),
                                     transfer);
        if (found == submitted.end()) {
            return LIBUSB_ERROR_NOT_FOUND;
        }
        canceled.insert(transfer);
    }
    mock_condition.notify_all();
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_handle_events_timeout_completed(
    libusb_context* context, timeval* timeout, int* completed) {
    CHECK(context == fake_context());
    static_cast<void>(completed);
    const auto duration = std::chrono::seconds(timeout->tv_sec) +
                          std::chrono::microseconds(timeout->tv_usec);
    libusb_transfer* transfer = nullptr;
    {
        std::unique_lock lock(mock_mutex);
        mock_condition.wait_for(lock, duration, [] {
            return std::any_of(submitted.begin(), submitted.end(),
                               transfer_ready);
        });
        const auto found = std::find_if(submitted.begin(), submitted.end(),
                                        transfer_ready);
        if (found == submitted.end()) {
            return LIBUSB_SUCCESS;
        }
        transfer = *found;
        submitted.erase(found);
        process_transfer(transfer);
    }
    transfer->callback(transfer);
    return LIBUSB_SUCCESS;
}

int main() {
    ViSession rm = VI_NULL;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);
    ViFindList list = VI_NULL;
    ViUInt32 count = 0;
    std::array<ViChar, VI_FIND_BUFLEN> found{};
    CHECK(viFindRsrc(rm, "USB?*::INSTR", &list, &count, found.data()) ==
          VI_SUCCESS);
    CHECK(count == 1);
    CHECK(std::string(found.data()) == kResource);
    CHECK(viClose(list) == VI_SUCCESS);
    CHECK(viFindRsrc(rm, "USB?*::RAW", &list, &count, found.data()) ==
          VI_SUCCESS);
    CHECK(count == 1);
    CHECK(std::string(found.data()) == kRawResource);
    CHECK(viClose(list) == VI_SUCCESS);

    ViSession first = VI_NULL;
    ViSession session = VI_NULL;
    CHECK(viOpen(rm, kResource, VI_NO_LOCK, 1000, &first) == VI_SUCCESS);
    CHECK(viOpen(rm, kResource, VI_NO_LOCK, 1000, &session) == VI_SUCCESS);
    CHECK(claims == 1);

    wrvisa_usb_raw_config_v1 raw_config{};
    raw_config.struct_size = sizeof(raw_config);
    raw_config.abi_major = WRVISA_USB_RAW_ABI_MAJOR;
    raw_config.abi_minor = WRVISA_USB_RAW_ABI_MINOR;
    raw_config.read_transfer_type = WRVISA_USB_TRANSFER_BULK;
    raw_config.read_endpoint = 0x81;
    raw_config.write_transfer_type = WRVISA_USB_TRANSFER_BULK;
    raw_config.write_endpoint = 0x02;
    CHECK(wrvisaSetUsbRawConfig(rm, kRawResource, &raw_config) == VI_SUCCESS);
    ViSession raw = VI_NULL;
    CHECK(viOpen(rm, kRawResource, VI_NO_LOCK, 1000, &raw) == VI_SUCCESS);
    CHECK(claims == 1);
    constexpr std::array<ViByte, 4> raw_payload{0xde, 0xad, 0xbe, 0xef};
    ViUInt32 raw_count = 0;
    CHECK(viWrite(raw, raw_payload.data(), raw_payload.size(), &raw_count) ==
          VI_SUCCESS);
    CHECK(raw_count == raw_payload.size());
    std::array<ViByte, 8> raw_buffer{};
    CHECK(viRead(raw, raw_buffer.data(), raw_payload.size(), &raw_count) ==
          VI_SUCCESS_MAX_CNT);
    CHECK(raw_count == raw_payload.size());
    CHECK(std::equal(raw_payload.begin(), raw_payload.end(),
                     raw_buffer.begin()));
    wrvisa_usb_control_request_v1 control{};
    control.struct_size = sizeof(control);
    control.abi_major = WRVISA_USB_RAW_ABI_MAJOR;
    control.abi_minor = WRVISA_USB_RAW_ABI_MINOR;
    control.request_type = 0xC0;
    control.request = 0x30;
    CHECK(wrvisaUsbControlTransfer(raw, &control, raw_buffer.data(), 4,
                                   &raw_count) == VI_SUCCESS);
    CHECK(raw_count == 4 && raw_buffer[0] == 1 && raw_buffer[3] == 4);
    control.request_type = 0x40;
    control.request = 0x31;
    CHECK(wrvisaUsbControlTransfer(raw, &control, raw_buffer.data(), 4,
                                   &raw_count) == VI_SUCCESS);
    CHECK(raw_count == 4);
    CHECK(viSetAttribute(raw, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    auto raw_object = session_object(raw);
    raw_count = 99;
    auto canceled_raw_read = std::async(std::launch::async, [&] {
        return viRead(raw, raw_buffer.data(), raw_buffer.size(), &raw_count);
    });
    wait_until_active(raw_object);
    CHECK(viTerminate(raw, VI_NULL, VI_NULL) == VI_SUCCESS);
    CHECK(canceled_raw_read.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
    CHECK(canceled_raw_read.get() == VI_ERROR_ABORT);
    CHECK(raw_count == 0);
    CHECK(viWrite(raw, raw_payload.data(), raw_payload.size(), &raw_count) ==
          VI_SUCCESS);
    CHECK(viRead(raw, raw_buffer.data(), raw_payload.size(), &raw_count) ==
          VI_SUCCESS_MAX_CNT);
    CHECK(std::equal(raw_payload.begin(), raw_payload.end(),
                     raw_buffer.begin()));
    CHECK(viClose(raw) == VI_SUCCESS);
    raw_object.reset();
    CHECK(releases == 0);
    CHECK(closes == 0);

    raw_config.read_transfer_type = WRVISA_USB_TRANSFER_INTERRUPT;
    raw_config.read_endpoint = 0x83;
    raw_config.write_transfer_type = WRVISA_USB_TRANSFER_INTERRUPT;
    raw_config.write_endpoint = 0x04;
    CHECK(wrvisaSetUsbRawConfig(rm, kRawResource, &raw_config) == VI_SUCCESS);
    ViSession raw_interrupt = VI_NULL;
    CHECK(viOpen(rm, kRawResource, VI_NO_LOCK, 1000, &raw_interrupt) ==
          VI_SUCCESS);
    constexpr std::array<ViByte, 2> interrupt_payload{0x12, 0x34};
    CHECK(viWrite(raw_interrupt, interrupt_payload.data(),
                  interrupt_payload.size(), &raw_count) == VI_SUCCESS);
    CHECK(raw_count == interrupt_payload.size());
    CHECK(viRead(raw_interrupt, raw_buffer.data(), interrupt_payload.size(),
                 &raw_count) == VI_SUCCESS_MAX_CNT);
    CHECK(raw_count == interrupt_payload.size());
    CHECK(std::equal(interrupt_payload.begin(), interrupt_payload.end(),
                     raw_buffer.begin()));
    CHECK(viClose(raw_interrupt) == VI_SUCCESS);
    CHECK(releases == 0);
    CHECK(closes == 0);
    query_identity(first);
    CHECK(viClose(first) == VI_SUCCESS);
    CHECK(releases == 0);
    CHECK(closes == 0);
    query_identity(session);

    ViUInt16 status_byte = 0;
    CHECK(viReadSTB(session, &status_byte) == VI_SUCCESS);
    CHECK(status_byte == 0x42);
    CHECK(viAssertTrigger(session, VI_TRIG_PROT_DEFAULT) == VI_SUCCESS);
    CHECK(triggers == 1);
    CHECK(viClear(session) == VI_SUCCESS);

    CHECK(viSetAttribute(session, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    auto object = session_object(session);
    std::array<ViByte, 16> canceled_buffer{};
    ViUInt32 canceled_count = 99;
    auto canceled_read = std::async(std::launch::async, [&] {
        return viRead(session, canceled_buffer.data(),
                      static_cast<ViUInt32>(canceled_buffer.size()),
                      &canceled_count);
    });
    wait_until_active(object);
    CHECK(viTerminate(session, VI_NULL, VI_NULL) == VI_SUCCESS);
    CHECK(canceled_read.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
    CHECK(canceled_read.get() == VI_ERROR_ABORT);
    CHECK(canceled_count == 0);
    query_identity(session);

    std::array<ViByte, 16> unplugged_buffer{};
    ViUInt32 unplugged_count = 99;
    auto unplugged_read = std::async(std::launch::async, [&] {
        return viRead(session, unplugged_buffer.data(),
                      static_cast<ViUInt32>(unplugged_buffer.size()),
                      &unplugged_count);
    });
    wait_until_active(object);
    {
        std::lock_guard lock(mock_mutex);
        connected = false;
    }
    CHECK(hotplug_callback != nullptr);
    CHECK(hotplug_callback(fake_context(), fake_device(),
                           LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                           hotplug_user_data) == 0);
    mock_condition.notify_all();
    CHECK(unplugged_read.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
    CHECK(unplugged_read.get() == VI_ERROR_CONN_LOST);
    CHECK(unplugged_count == 0);

    CHECK(viClose(session) == VI_SUCCESS);
    object.reset();
    CHECK(releases == 1);
    CHECK(closes == 1);
    CHECK(viClose(rm) == VI_SUCCESS);
    return 0;
}
