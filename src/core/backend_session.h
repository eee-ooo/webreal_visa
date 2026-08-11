#ifndef WRVISA_CORE_BACKEND_SESSION_H
#define WRVISA_CORE_BACKEND_SESSION_H

#include <cstddef>

#include "runtime/operation.h"
#include "visa.h"

namespace wrvisa {

struct ReadOptions {
    bool termchar_enabled{false};
    ViByte termchar{0};
};

class BackendSession {
public:
    virtual ~BackendSession() = default;
    virtual ViStatus read(Operation& operation, ViPBuf buffer, ViUInt32 count,
                          ViPUInt32 return_count, ReadOptions options) = 0;
    virtual ViStatus write(Operation& operation, ViConstBuf buffer, ViUInt32 count,
                           ViPUInt32 return_count) = 0;
    virtual ViStatus clear() = 0;
    virtual ViStatus flush(ViUInt16 mask) = 0;
    virtual ViStatus read_stb(ViPUInt16 status) = 0;
    virtual ViStatus assert_trigger(ViUInt16 protocol) {
        static_cast<void>(protocol);
        return VI_ERROR_NSUP_OPER;
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
