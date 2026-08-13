#include <algorithm>
#include <string>

#include "backends/usb/libusb_provider.h"
#include "backends/usb/usb_provider.h"
#include "resource/resource_parser.h"
#include "test_support.h"

int main() {
    using wrvisa::libusb_detail::EndpointDescriptor;
    using wrvisa::libusb_detail::InterfaceDescriptor;
    using wrvisa::libusb_detail::inspect_interface;

    InterfaceDescriptor usbtmc{
        3, 0, 0xfe, 0x03, 0,
        std::vector<EndpointDescriptor>{{0x02, 0x02, 64},
                                        {0x81, 0x02, 64}}};
    auto info = inspect_interface(usbtmc);
    CHECK(info.has_value());
    CHECK(info->interface_number == 3);
    CHECK(info->bulk_out_endpoint == 0x02);
    CHECK(info->bulk_in_endpoint == 0x81);
    CHECK(info->bulk_in_max_packet_size == 64);
    CHECK(!info->usb488);

    auto usb488 = usbtmc;
    usb488.alternate_setting = 2;
    usb488.interface_protocol = 1;
    usb488.endpoints.push_back({0x83, 0x03, 8});
    info = inspect_interface(usb488);
    CHECK(info.has_value());
    CHECK(info->usb488);
    CHECK(info->interrupt_in_endpoint == 0x83);
    CHECK(info->alternate_setting == 2);

    auto invalid = usbtmc;
    invalid.interface_class = 0xff;
    CHECK(!inspect_interface(invalid));

    InterfaceDescriptor vendor_interface{
        4, 1, 0xff, 0, 0,
        std::vector<EndpointDescriptor>{{0x02, 0x02, 64},
                                        {0x81, 0x02, 512},
                                        {0x04, 0x03, 16},
                                        {0x83, 0x03, 16}}};
    const wrvisa::UsbRawConfiguration raw_bulk{
        1, wrvisa::UsbTransferType::bulk, 0x81,
        wrvisa::UsbTransferType::bulk, 0x02};
    info = wrvisa::libusb_detail::inspect_raw_interface(vendor_interface,
                                                        raw_bulk);
    CHECK(info.has_value());
    CHECK(info->interface_number == 4);
    CHECK(info->bulk_in_endpoint == 0x81);
    CHECK(info->bulk_in_max_packet_size == 512);
    CHECK(info->bulk_out_endpoint == 0x02);
    const wrvisa::UsbRawConfiguration raw_interrupt{
        1, wrvisa::UsbTransferType::interrupt, 0x83,
        wrvisa::UsbTransferType::interrupt, 0x04};
    info = wrvisa::libusb_detail::inspect_raw_interface(vendor_interface,
                                                        raw_interrupt);
    CHECK(info.has_value());
    CHECK(info->interrupt_in_endpoint == 0x83);
    CHECK(info->interrupt_out_endpoint == 0x04);
    auto bad_raw = raw_bulk;
    bad_raw.alternate_setting = 0;
    CHECK(!wrvisa::libusb_detail::inspect_raw_interface(vendor_interface,
                                                        bad_raw));
    bad_raw = raw_bulk;
    bad_raw.read_endpoint = 0x82;
    CHECK(!wrvisa::libusb_detail::inspect_raw_interface(vendor_interface,
                                                        bad_raw));
    invalid = usbtmc;
    invalid.interface_protocol = 2;
    CHECK(!inspect_interface(invalid));
    invalid = usbtmc;
    invalid.endpoints.pop_back();
    CHECK(!inspect_interface(invalid));
    invalid = usbtmc;
    invalid.endpoints.push_back({0x82, 0x02, 64});
    CHECK(!inspect_interface(invalid));
    invalid = usbtmc;
    invalid.endpoints[1].maximum_packet_size = 0;
    CHECK(!inspect_interface(invalid));

    using wrvisa::libusb_detail::map_error;
    CHECK(map_error(0, false) == VI_SUCCESS);
    CHECK(map_error(-2, false) == VI_ERROR_INV_PARAMETER);
    CHECK(map_error(-3, true) == VI_ERROR_NPERMISSION);
    CHECK(map_error(-4, true) == VI_ERROR_RSRC_NFOUND);
    CHECK(map_error(-4, false) == VI_ERROR_CONN_LOST);
    CHECK(map_error(-6, false) == VI_ERROR_RSRC_BUSY);
    CHECK(map_error(-7, false) == VI_ERROR_TMO);
    CHECK(map_error(-11, false) == VI_ERROR_ALLOC);
    CHECK(map_error(-12, false) == VI_ERROR_NSUP_OPER);
    CHECK(map_error(-99, false) == VI_ERROR_IO);

    for (const auto& resource : wrvisa::discover_usb_resources()) {
        const auto parsed = wrvisa::parse_resource(resource);
        CHECK(parsed.has_value());
        CHECK(parsed->kind == wrvisa::ResourceKind::usb_instr ||
              parsed->kind == wrvisa::ResourceKind::usb_raw);
    }

    const auto missing = wrvisa::parse_resource(
        "USB0::0xFFFF::0xFFFF::WRVISA-NOT-A-DEVICE::0::INSTR");
    CHECK(missing.has_value());
    ViStatus status = VI_SUCCESS;
    auto transport = wrvisa::open_usb_transport(*missing, 100, status);
    CHECK(!transport);
    if (wrvisa::libusb_detail::provider_available()) {
        // A sandbox may deny libusb enumeration before it can prove that an
        // impossible VID/PID is absent. Preserve that diagnostic rather than
        // making this test depend on host USB visibility.
        CHECK(status == VI_ERROR_RSRC_NFOUND ||
              status == VI_ERROR_NPERMISSION || status == VI_ERROR_IO);
    } else {
        CHECK(status == VI_ERROR_NSUP_OPER);
    }
    return 0;
}
