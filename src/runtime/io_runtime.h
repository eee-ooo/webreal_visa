#ifndef WRVISA_RUNTIME_IO_RUNTIME_H
#define WRVISA_RUNTIME_IO_RUNTIME_H

#include <memory>
#include <thread>
#include <vector>

#include <asio.hpp>

namespace wrvisa {

class IoRuntime final {
public:
    IoRuntime();
    ~IoRuntime();

    IoRuntime(const IoRuntime&) = delete;
    IoRuntime& operator=(const IoRuntime&) = delete;

    asio::any_io_executor executor() noexcept { return context_.get_executor(); }

private:
    asio::io_context context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::vector<std::thread> workers_;
};

std::shared_ptr<IoRuntime> shared_io_runtime();

}  // namespace wrvisa

#endif
