#include "resource/resource_parser.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

#include "webreal_visa_ext.h"

namespace wrvisa {
namespace {

std::string upper(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return result;
}

std::vector<std::string_view> split(std::string_view value) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    bool bracketed = false;
    for (std::size_t index = 0; index <= value.size(); ++index) {
        if (index < value.size() && value[index] == '[') {
            if (bracketed) {
                return {};
            }
            bracketed = true;
            continue;
        }
        if (index < value.size() && value[index] == ']') {
            if (!bracketed) {
                return {};
            }
            bracketed = false;
            continue;
        }
        const bool separator = !bracketed && index + 1u < value.size() &&
                               value[index] == ':' && value[index + 1u] == ':';
        const bool at_end = index == value.size();
        if (!separator && !at_end) {
            continue;
        }
        const auto end = index;
        const auto part = value.substr(start, end == std::string_view::npos
                                                  ? value.size() - start
                                                  : end - start);
        if (part.empty()) {
            return {};
        }
        parts.push_back(part);
        if (at_end) {
            break;
        }
        start = index + 2u;
        ++index;
    }
    if (bracketed) {
        return {};
    }
    return parts;
}

std::optional<ViUInt16> parse_decimal(std::string_view value, ViUInt32 maximum) {
    if (value.empty()) {
        return std::nullopt;
    }
    ViUInt32 parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed > maximum) {
        return std::nullopt;
    }
    return static_cast<ViUInt16>(parsed);
}

std::optional<ViUInt16> parse_hex_id(std::string_view value) {
    if (value.size() != 6 || value[0] != '0' ||
        (value[1] != 'x' && value[1] != 'X')) {
        return std::nullopt;
    }
    ViUInt32 parsed = 0;
    const auto result =
        std::from_chars(value.data() + 2, value.data() + value.size(), parsed, 16);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed > std::numeric_limits<ViUInt16>::max()) {
        return std::nullopt;
    }
    return static_cast<ViUInt16>(parsed);
}

std::optional<ViUInt16> parse_prefix(std::string_view token, std::string_view name) {
    if (token.size() < name.size() || upper(token.substr(0, name.size())) != name) {
        return std::nullopt;
    }
    const auto suffix = token.substr(name.size());
    if (suffix.empty()) {
        return ViUInt16{0};
    }
    return parse_decimal(suffix, std::numeric_limits<ViUInt16>::max());
}

std::string prefix(std::string_view name, ViUInt16 board) {
    return std::string(name) + std::to_string(board);
}

std::optional<ResourceDescriptor> parse_asrl(const std::vector<std::string_view>& parts) {
    const auto board = parse_prefix(parts[0], "ASRL");
    if (!board || parts.size() > 2 ||
        (parts.size() == 2 && upper(parts[1]) != "INSTR")) {
        return std::nullopt;
    }
    return ResourceDescriptor{ResourceKind::asrl_instr, VI_INTF_ASRL, *board, "INSTR",
                              prefix("ASRL", *board) + "::INSTR", {}, 0, {}};
}

std::optional<ResourceDescriptor> parse_gpib(const std::vector<std::string_view>& parts) {
    const auto board = parse_prefix(parts[0], "GPIB");
    if (!board || parts.size() < 2 || parts.size() > 4) {
        return std::nullopt;
    }
    if (parts.size() == 2 && upper(parts[1]) == "INTFC") {
        return ResourceDescriptor{ResourceKind::gpib_intfc, VI_INTF_GPIB, *board,
                                  "INTFC", prefix("GPIB", *board) + "::INTFC",
                                  {}, 0, {}};
    }
    const auto primary = parse_decimal(parts[1], 30);
    if (!primary) {
        return std::nullopt;
    }
    std::optional<ViUInt16> secondary;
    if (parts.size() >= 3 && upper(parts[2]) != "INSTR") {
        secondary = parse_decimal(parts[2], 30);
        if (!secondary) {
            return std::nullopt;
        }
    }
    const auto expected_size = secondary ? 4u : 3u;
    if (parts.size() == expected_size && upper(parts.back()) != "INSTR") {
        return std::nullopt;
    }
    if (parts.size() > expected_size || (parts.size() == 4 && !secondary)) {
        return std::nullopt;
    }
    auto canonical = prefix("GPIB", *board) + "::" + std::to_string(*primary);
    if (secondary) {
        canonical += "::" + std::to_string(*secondary);
    }
    canonical += "::INSTR";
    return ResourceDescriptor{ResourceKind::gpib_instr, VI_INTF_GPIB, *board,
                              "INSTR", std::move(canonical), {}, 0, {}};
}

std::optional<ResourceDescriptor> parse_tcpip(const std::vector<std::string_view>& parts) {
    const auto board = parse_prefix(parts[0], "TCPIP");
    if (!board || parts.size() < 2 || parts.size() > 4 || parts[1].empty()) {
        return std::nullopt;
    }
    const std::string display_host(parts[1]);
    std::string host = display_host;
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2u);
    } else if (host.find(':') != std::string::npos) {
        return std::nullopt;
    }
    if (host.empty()) {
        return std::nullopt;
    }
    if (parts.size() == 4 && upper(parts[3]) == "SOCKET") {
        const auto port = parse_decimal(parts[2], std::numeric_limits<ViUInt16>::max());
        if (!port || *port == 0) {
            return std::nullopt;
        }
        return ResourceDescriptor{ResourceKind::tcpip_socket, VI_INTF_TCPIP, *board,
                                  "SOCKET", prefix("TCPIP", *board) + "::" +
                                                display_host + "::" +
                                                std::to_string(*port) + "::SOCKET",
                                  std::move(host), *port, {}};
    }
    std::string device = "inst0";
    if (parts.size() == 3 && upper(parts[2]) != "INSTR") {
        device = std::string(parts[2]);
    } else if (parts.size() == 4) {
        if (upper(parts[3]) != "INSTR") {
            return std::nullopt;
        }
        device = std::string(parts[2]);
    } else if (parts.size() == 3 && upper(parts[2]) != "INSTR") {
        return std::nullopt;
    }
    return ResourceDescriptor{ResourceKind::tcpip_instr, VI_INTF_TCPIP, *board,
                              "INSTR", prefix("TCPIP", *board) + "::" +
                                           display_host + "::" + device + "::INSTR",
                              std::move(host), 0, std::move(device)};
}

std::optional<ResourceDescriptor> parse_usb(const std::vector<std::string_view>& parts) {
    const auto board = parse_prefix(parts[0], "USB");
    if (!board || parts.size() < 4 || parts.size() > 6) {
        return std::nullopt;
    }
    const auto vendor = parse_hex_id(parts[1]);
    const auto product = parse_hex_id(parts[2]);
    if (!vendor || !product || parts[3].empty()) {
        return std::nullopt;
    }
    ViUInt16 interface_number = 0;
    bool has_interface = false;
    if (parts.size() >= 5 && upper(parts[4]) != "INSTR") {
        const auto parsed = parse_decimal(parts[4], std::numeric_limits<ViUInt16>::max());
        if (!parsed) {
            return std::nullopt;
        }
        interface_number = *parsed;
        has_interface = true;
    }
    const auto expected_size = has_interface ? 6u : 5u;
    if (parts.size() == expected_size && upper(parts.back()) != "INSTR") {
        return std::nullopt;
    }
    if (parts.size() > expected_size || (parts.size() == 6 && !has_interface)) {
        return std::nullopt;
    }
    std::ostringstream canonical;
    canonical << "USB" << *board << "::0x" << std::uppercase << std::hex
              << std::setw(4) << std::setfill('0') << *vendor << "::0x" << std::setw(4)
              << *product << "::" << parts[3] << std::dec << "::" << interface_number
              << "::INSTR";
    return ResourceDescriptor{ResourceKind::usb_instr, VI_INTF_USB, *board, "INSTR",
                              canonical.str(), {}, 0, {}};
}

}  // namespace

std::optional<ResourceDescriptor> parse_resource(std::string_view value) {
    if (value.empty() || value.size() >= VI_FIND_BUFLEN) {
        return std::nullopt;
    }
    if (upper(value) == WRVISA_MOCK_RESOURCE) {
        return ResourceDescriptor{ResourceKind::project_mock, WRVISA_INTF_MOCK, 0,
                                  "INSTR", WRVISA_MOCK_RESOURCE, {}, 0, {}};
    }
    const auto parts = split(value);
    if (parts.empty()) {
        return std::nullopt;
    }
    const auto first = upper(parts[0]);
    if (first.rfind("ASRL", 0) == 0) {
        return parse_asrl(parts);
    }
    if (first.rfind("GPIB", 0) == 0) {
        return parse_gpib(parts);
    }
    if (first.rfind("TCPIP", 0) == 0) {
        return parse_tcpip(parts);
    }
    if (first.rfind("USB", 0) == 0) {
        return parse_usb(parts);
    }
    return std::nullopt;
}

}  // namespace wrvisa
