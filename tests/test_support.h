#ifndef WRVISA_TEST_SUPPORT_H
#define WRVISA_TEST_SUPPORT_H

#include <cstdlib>
#include <iostream>
#include <string_view>

inline void test_check(bool condition, std::string_view expression,
                       std::string_view file, int line) {
    if (!condition) {
        std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
        std::exit(1);
    }
}

#define CHECK(expression) \
    test_check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#endif
