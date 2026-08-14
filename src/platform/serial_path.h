#ifndef WRVISA_PLATFORM_SERIAL_PATH_H
#define WRVISA_PLATFORM_SERIAL_PATH_H

#include <string>
#include <string_view>

namespace wrvisa {

inline std::string platform_serial_path(std::string_view input) {
#if defined(_WIN32)
    if (input.size() > 3 && input.substr(0, 3) == "COM") {
        unsigned int number = 0;
        bool numeric = true;
        for (const char value : input.substr(3)) {
            if (value < '0' || value > '9') {
                numeric = false;
                break;
            }
            number = number * 10u + static_cast<unsigned int>(value - '0');
        }
        if (numeric && number >= 10u) {
            return "\\\\.\\" + std::string(input);
        }
    }
#endif
    return std::string(input);
}

}  // namespace wrvisa

#endif
