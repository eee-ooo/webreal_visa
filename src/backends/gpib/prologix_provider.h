#ifndef WRVISA_BACKENDS_GPIB_PROLOGIX_PROVIDER_H
#define WRVISA_BACKENDS_GPIB_PROLOGIX_PROVIDER_H

#include <cstdint>
#include <memory>
#include <string>

#include "backends/gpib/gpib_provider.h"

namespace wrvisa {

enum class PrologixConnectionKind : std::uint16_t {
    serial = 1,
    tcp = 2,
};

struct PrologixConfiguration {
    PrologixConnectionKind connection{PrologixConnectionKind::serial};
    std::string endpoint;
    std::uint16_t tcp_port{0};
    std::uint8_t eot_char{0};
    std::uint32_t read_timeout_ms{0};
    std::uint32_t maximum_response_size{0};
};

// Returns an RM-owned provider while sharing the exact configured endpoint
// identity across matching RMs in this process. A live endpoint identity with
// different protocol settings is rejected with VI_ERROR_RSRC_BUSY.
std::shared_ptr<GpibProvider> make_prologix_provider(
    ViUInt16 board, PrologixConfiguration configuration, ViStatus& status);

}  // namespace wrvisa

#endif
