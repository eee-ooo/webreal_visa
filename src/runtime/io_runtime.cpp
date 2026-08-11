#include "runtime/io_runtime.h"

#include <algorithm>
#include <mutex>

namespace wrvisa {

IoRuntime::IoRuntime() : work_(asio::make_work_guard(context_)) {
    const auto reported = std::thread::hardware_concurrency();
    const auto worker_count = std::clamp(reported == 0 ? 2u : reported, 2u, 4u);
    workers_.reserve(worker_count);
    try {
        for (unsigned int index = 0; index < worker_count; ++index) {
            workers_.emplace_back([this] { context_.run(); });
        }
    } catch (...) {
        work_.reset();
        context_.stop();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

IoRuntime::~IoRuntime() {
    work_.reset();
    context_.stop();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::shared_ptr<IoRuntime> shared_io_runtime() {
    static std::mutex mutex;
    static std::weak_ptr<IoRuntime> runtime;
    std::lock_guard lock(mutex);
    auto shared = runtime.lock();
    if (!shared) {
        shared = std::make_shared<IoRuntime>();
        runtime = shared;
    }
    return shared;
}

}  // namespace wrvisa
