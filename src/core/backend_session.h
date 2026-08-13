#ifndef WRVISA_CORE_BACKEND_SESSION_H
#define WRVISA_CORE_BACKEND_SESSION_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "runtime/operation.h"
#include "visa.h"

namespace wrvisa {

struct ReadOptions {
    bool termchar_enabled{false};
    ViByte termchar{0};
};

struct WriteOptions {
    bool send_end{true};
};

class BackendSession {
public:
    virtual ~BackendSession() = default;
    virtual ViStatus read(Operation& operation, ViPBuf buffer, ViUInt32 count,
                          ViPUInt32 return_count, ReadOptions options) = 0;
    virtual ViStatus write(Operation& operation, ViConstBuf buffer, ViUInt32 count,
                           ViPUInt32 return_count, WriteOptions options) = 0;
    virtual ViStatus clear() = 0;
    virtual ViStatus flush(ViUInt16 mask) = 0;
    virtual ViStatus read_stb(ViPUInt16 status) = 0;
    virtual ViStatus assert_trigger(ViUInt16 protocol) {
        static_cast<void>(protocol);
        return VI_ERROR_NSUP_OPER;
    }
    virtual ViStatus clear(Operation& operation) {
        static_cast<void>(operation);
        return clear();
    }
    virtual ViStatus flush(Operation& operation, ViUInt16 mask) {
        static_cast<void>(operation);
        return flush(mask);
    }
    virtual ViStatus read_stb(Operation& operation, ViPUInt16 status) {
        static_cast<void>(operation);
        return read_stb(status);
    }
    virtual ViStatus assert_trigger(Operation& operation, ViUInt16 protocol) {
        static_cast<void>(operation);
        return assert_trigger(protocol);
    }
    virtual ViStatus usb_control(Operation& operation,
                                 std::uint8_t request_type,
                                 std::uint8_t request,
                                 std::uint16_t value,
                                 std::uint16_t index, ViBuf data,
                                 ViUInt32 count,
                                 ViPUInt32 return_count) {
        static_cast<void>(operation);
        static_cast<void>(request_type);
        static_cast<void>(request);
        static_cast<void>(value);
        static_cast<void>(index);
        static_cast<void>(data);
        static_cast<void>(count);
        *return_count = 0;
        return VI_ERROR_NSUP_OPER;
    }
    virtual ViStatus lock(Operation& operation, ViAccessMode lock_type,
                          const std::string& access_key) {
        static_cast<void>(operation);
        static_cast<void>(lock_type);
        static_cast<void>(access_key);
        return VI_SUCCESS;
    }
    virtual ViStatus unlock(Operation& operation) {
        static_cast<void>(operation);
        return VI_SUCCESS;
    }
    virtual ViStatus set_attribute(ViAttr attribute, ViAttrState value) {
        static_cast<void>(attribute);
        static_cast<void>(value);
        return VI_ERROR_NSUP_ATTR;
    }
    virtual ViStatus get_attribute(ViAttr attribute, void* value) {
        static_cast<void>(attribute);
        static_cast<void>(value);
        return VI_ERROR_NSUP_ATTR;
    }
    virtual void close() noexcept {}
    virtual void notify_cancel() noexcept = 0;
};

}  // namespace wrvisa

#endif
