#include <string>

#include "resource/find_expression.h"
#include "resource/resource_parser.h"
#include "test_support.h"
#include "webreal_visa_ext.h"

int main() {
    using wrvisa::ResourceKind;

    const auto asrl = wrvisa::parse_resource("asrl2");
    CHECK(asrl.has_value());
    CHECK(asrl->kind == ResourceKind::asrl_instr);
    CHECK(asrl->canonical_name == "ASRL2::INSTR");

    const auto gpib = wrvisa::parse_resource("GPIB::4::7");
    CHECK(gpib.has_value());
    CHECK(gpib->canonical_name == "GPIB0::4::7::INSTR");

    const auto intfc = wrvisa::parse_resource("gpib3::intfc");
    CHECK(intfc.has_value());
    CHECK(intfc->resource_class == "INTFC");

    const auto tcpip = wrvisa::parse_resource("TCPIP::example.test::5025::SOCKET");
    CHECK(tcpip.has_value());
    CHECK(tcpip->kind == ResourceKind::tcpip_socket);
    CHECK(tcpip->canonical_name == "TCPIP0::example.test::5025::SOCKET");

    const auto ipv6 = wrvisa::parse_resource("TCPIP2::[::1]::9000::SOCKET");
    CHECK(ipv6.has_value());
    CHECK(ipv6->host == "::1");
    CHECK(ipv6->port == 9000);
    CHECK(ipv6->canonical_name == "TCPIP2::[::1]::9000::SOCKET");

    const auto usb = wrvisa::parse_resource("USB1::0x1234::0xabcd::SERIAL::0");
    CHECK(usb.has_value());
    CHECK(usb->canonical_name == "USB1::0x1234::0xABCD::SERIAL::0::INSTR");

    const auto mock = wrvisa::parse_resource("wrvisa0::mock::instr");
    CHECK(mock.has_value());
    CHECK(mock->kind == ResourceKind::project_mock);
    CHECK(mock->canonical_name == WRVISA_MOCK_RESOURCE);

    CHECK(!wrvisa::parse_resource("GPIB0::31::INSTR"));
    CHECK(!wrvisa::parse_resource("USB0::1234::0x5678::SERIAL"));
    CHECK(!wrvisa::parse_resource("TCPIP0::::INSTR"));

    const auto vxi = wrvisa::parse_resource("TCPIP0::host::inst0::INSTR");
    CHECK(vxi.has_value());
    CHECK(vxi->tcpip_protocol == wrvisa::TcpipProtocol::vxi11);
    const auto hislip = wrvisa::parse_resource("TCPIP0::host::hislip0::INSTR");
    CHECK(hislip.has_value());
    CHECK(hislip->tcpip_protocol == wrvisa::TcpipProtocol::hislip);
    const auto unsupported = wrvisa::parse_resource(
        "TCPIP0::host::vendor0::INSTR");
    CHECK(unsupported.has_value());
    CHECK(unsupported->tcpip_protocol == wrvisa::TcpipProtocol::unsupported);
    CHECK(!wrvisa::parse_resource("TCPIP0::127.0.0.1::0::SOCKET"));

    std::string error;
    const auto all_instr = wrvisa::FindExpression::compile("?*INSTR", error);
    CHECK(all_instr.has_value());
    CHECK(all_instr->matches(WRVISA_MOCK_RESOURCE));
    CHECK(!all_instr->matches("GPIB0::INTFC"));

    const auto alternate = wrvisa::FindExpression::compile("(ASRL|TCPIP)?*::INSTR", error);
    CHECK(alternate.has_value());
    CHECK(alternate->matches("ASRL0::INSTR"));
    CHECK(alternate->matches("TCPIP0::host::inst0::INSTR"));
    CHECK(!alternate->matches("USB0::INSTR"));

    const auto character_class =
        wrvisa::FindExpression::compile("GPIB[0-3]::[1-9]::INSTR", error);
    CHECK(character_class.has_value());
    CHECK(character_class->matches("GPIB2::7::INSTR"));
    CHECK(!character_class->matches("GPIB8::7::INSTR"));

    const auto numeric_filter = wrvisa::FindExpression::compile(
        "?*{VI_ATTR_INTF_TYPE == 4 && VI_ATTR_INTF_NUM >= 2}", error);
    CHECK(numeric_filter.has_value());
    CHECK(numeric_filter->matches(*asrl));
    CHECK(!numeric_filter->matches(*mock));

    const auto string_filter = wrvisa::FindExpression::compile(
        "WRVISA0?*{VI_ATTR_RSRC_CLASS == \"INSTR\" && "
        "VI_ATTR_RSRC_NAME != \"ASRL2::INSTR\"}", error);
    CHECK(string_filter.has_value());
    CHECK(string_filter->matches(*mock));

    const auto logical_filter = wrvisa::FindExpression::compile(
        "?*{!(VI_ATTR_INTF_NUM < 2) || VI_ATTR_INTF_TYPE == 0x8000}", error);
    CHECK(logical_filter.has_value());
    CHECK(logical_filter->matches(*asrl));
    CHECK(logical_filter->matches(*mock));

    const auto baud_filter = wrvisa::FindExpression::compile(
        "ASRL?*{VI_ATTR_ASRL_BAUD == 9600}", error);
    CHECK(baud_filter.has_value());
    CHECK(baud_filter->matches(*asrl));

    CHECK(!wrvisa::FindExpression::compile(
        "?*{VI_ATTR_TCPIP_PORT == 5025}", error));
    CHECK(!wrvisa::FindExpression::compile(
        "?*{VI_ATTR_RSRC_CLASS > \"INSTR\"}", error));
    CHECK(!wrvisa::FindExpression::compile(
        "?*{VI_ATTR_INTF_NUM == \"0\"}", error));
    CHECK(!wrvisa::FindExpression::compile("?*{VI_ATTR_INTF_NUM == 0", error));
    CHECK(!wrvisa::FindExpression::compile("?*{}", error));
    CHECK(!wrvisa::FindExpression::compile("[abc", error));
    CHECK(!wrvisa::FindExpression::compile("a**", error));
    return 0;
}
