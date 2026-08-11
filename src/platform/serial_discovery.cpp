#include "platform/serial_discovery.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <set>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace wrvisa {
namespace {

#if !defined(_WIN32)
bool decimal_suffix(std::string_view value, std::string_view prefix,
                    ViUInt16& result) {
    if (!value.starts_with(prefix) || value.size() == prefix.size()) {
        return false;
    }
    unsigned long parsed = 0;
    for (const char character : value.substr(prefix.size())) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isdigit(byte) == 0) {
            return false;
        }
        parsed = parsed * 10ul + static_cast<unsigned long>(character - '0');
        if (parsed > std::numeric_limits<ViUInt16>::max()) {
            return false;
        }
    }
    result = static_cast<ViUInt16>(parsed);
    return true;
}

bool serial_candidate(std::string_view name) {
#if defined(__APPLE__)
    return name.starts_with("cu.");
#else
    return name.starts_with("ttyS") || name.starts_with("ttyUSB") ||
           name.starts_with("ttyACM") || name.starts_with("ttyAMA") ||
           name.starts_with("rfcomm");
#endif
}
#endif

}  // namespace

std::map<ViUInt16, std::string> discover_serial_ports() noexcept {
    std::map<ViUInt16, std::string> result;
    try {
#if defined(_WIN32)
        std::vector<char> target(32768);
        for (unsigned int number = 1; number <= 256u; ++number) {
            const auto name = "COM" + std::to_string(number);
            if (QueryDosDeviceA(name.c_str(), target.data(),
                                static_cast<DWORD>(target.size())) != 0) {
                result.emplace(static_cast<ViUInt16>(number), name);
            }
        }
#else
        std::vector<std::string> paths;
        std::error_code error;
        for (std::filesystem::directory_iterator iterator("/dev", error), end;
             !error && iterator != end; iterator.increment(error)) {
            const auto name = iterator->path().filename().string();
            if (serial_candidate(name)) {
                paths.push_back(iterator->path().string());
            }
        }
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

        std::set<ViUInt16> assigned;
        std::vector<std::string> unassigned;
        for (const auto& path : paths) {
            ViUInt16 number = 0;
            const auto name = std::filesystem::path(path).filename().string();
            if (decimal_suffix(name, "ttyS", number) && !assigned.contains(number)) {
                result.emplace(number, path);
                assigned.insert(number);
            } else {
                unassigned.push_back(path);
            }
        }
        ViUInt32 candidate = 0;
        for (const auto& path : unassigned) {
            while (candidate <= std::numeric_limits<ViUInt16>::max() &&
                   assigned.contains(static_cast<ViUInt16>(candidate))) {
                ++candidate;
            }
            if (candidate > std::numeric_limits<ViUInt16>::max()) {
                break;
            }
            const auto number = static_cast<ViUInt16>(candidate);
            result.emplace(number, path);
            assigned.insert(number);
            ++candidate;
        }
#endif
    } catch (...) {
        // Discovery is best effort. Explicit wrvisaSetSerialPath remains usable.
    }
    return result;
}

}  // namespace wrvisa
