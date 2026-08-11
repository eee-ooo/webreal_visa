#ifndef WRVISA_BACKENDS_MOCK_SESSION_H
#define WRVISA_BACKENDS_MOCK_SESSION_H

#include <condition_variable>
#include <deque>
#include <mutex>

#include "core/backend_session.h"

namespace wrvisa {

class MockBackendSession final : public BackendSession {
public:
    ViStatus read(Operation& operation, ViPBuf buffer, ViUInt32 count,
                  ViPUInt32 return_count, ReadOptions options) override;
    ViStatus write(Operation& operation, ViConstBuf buffer, ViUInt32 count,
                   ViPUInt32 return_count) override;
    ViStatus clear() override;
    ViStatus flush(ViUInt16 mask) override;
    ViStatus read_stb(ViPUInt16 status) override;
    ViStatus assert_trigger(ViUInt16 protocol) override;
    void notify_cancel() noexcept override;

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<ViByte> incoming_;
};

}  // namespace wrvisa

#endif
