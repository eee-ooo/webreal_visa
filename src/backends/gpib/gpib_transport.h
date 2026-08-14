#ifndef WRVISA_BACKENDS_GPIB_GPIB_TRANSPORT_H
#define WRVISA_BACKENDS_GPIB_GPIB_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "runtime/operation.h"

namespace wrvisa {

struct GpibCapabilities {
    bool send_end{false};
    bool device_clear{false};
    bool trigger{false};
    bool serial_poll{false};
};

// Addressed-instrument transport contract. Implementations own controller and
// driver handles; protocol sessions never depend on linux-gpib, NI-488.2, or
// a controller-specific command set.
class GpibTransport {
public:
    virtual ~GpibTransport() = default;

    virtual GpibCapabilities capabilities() const noexcept = 0;
    virtual ViStatus read(Operation& operation, std::size_t maximum_size,
                          std::vector<std::uint8_t>& data, bool& end) = 0;
    virtual ViStatus write(Operation& operation,
                           std::span<const std::uint8_t> data, bool send_end,
                           std::size_t& transferred) = 0;
    virtual ViStatus clear(Operation& operation) {
        static_cast<void>(operation);
        return VI_ERROR_NSUP_OPER;
    }
    virtual ViStatus trigger(Operation& operation) {
        static_cast<void>(operation);
        return VI_ERROR_NSUP_OPER;
    }
    virtual ViStatus serial_poll(Operation& operation,
                                 std::uint8_t& status_byte) {
        static_cast<void>(operation);
        static_cast<void>(status_byte);
        return VI_ERROR_NSUP_OPER;
    }
    // Discard only bytes already prefetched into the transport. This must not
    // perform device I/O and is called while the session I/O mutex is held.
    virtual void discard_read_buffer() noexcept = 0;
    virtual void cancel() noexcept = 0;
    virtual void close() noexcept = 0;
};

}  // namespace wrvisa

#endif
