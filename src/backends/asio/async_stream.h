#ifndef WRVISA_BACKENDS_ASIO_ASYNC_STREAM_H
#define WRVISA_BACKENDS_ASIO_ASYNC_STREAM_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "core/backend_session.h"
#include "runtime/io_runtime.h"

namespace wrvisa {

template <typename Stream>
class AsyncStreamBackend : public BackendSession {
public:
    AsyncStreamBackend()
        : runtime_(shared_io_runtime()),
          strand_(asio::make_strand(runtime_->executor())),
          stream_(strand_) {}

    ~AsyncStreamBackend() override { close(); }

    AsyncStreamBackend(const AsyncStreamBackend&) = delete;
    AsyncStreamBackend& operator=(const AsyncStreamBackend&) = delete;

    ViStatus read(Operation& operation, ViPBuf buffer, ViUInt32 count,
                  ViPUInt32 return_count, ReadOptions options) final {
        *return_count = 0;
        auto request = std::make_shared<ReadRequest>(strand_, operation, count, options);
        auto future = request->completion.get_future();
        std::function<void()> cancel = [this, weak = std::weak_ptr<ReadRequest>(request)] {
            asio::post(strand_, [this, weak] {
                if (const auto locked = weak.lock()) {
                    cancel_read(locked);
                }
            });
        };
        operation.set_cancel_handler(std::move(cancel));
        try {
            asio::post(strand_, [this, request] { enqueue_read(request); });
        } catch (...) {
            operation.clear_cancel_handler();
            throw;
        }

        auto result = future.get();
        operation.clear_cancel_handler();
        if (result.status >= VI_SUCCESS) {
            if (!result.data.empty()) {
                std::memcpy(buffer, result.data.data(), result.data.size());
            }
            *return_count = static_cast<ViUInt32>(result.data.size());
        }
        return result.status;
    }

    ViStatus write(Operation& operation, ViConstBuf buffer, ViUInt32 count,
                   ViPUInt32 return_count, WriteOptions options) final {
        static_cast<void>(options);
        *return_count = 0;
        auto request = std::make_shared<WriteRequest>(
            strand_, operation, std::vector<ViByte>(buffer, buffer + count));
        auto future = request->completion.get_future();
        std::function<void()> cancel =
            [this, weak = std::weak_ptr<WriteRequest>(request)] {
                asio::post(strand_, [this, weak] {
                    if (const auto locked = weak.lock()) {
                        cancel_write(locked);
                    }
                });
            };
        operation.set_cancel_handler(std::move(cancel));
        try {
            asio::post(strand_, [this, request] { enqueue_write(request); });
        } catch (...) {
            operation.clear_cancel_handler();
            throw;
        }

        const auto result = future.get();
        operation.clear_cancel_handler();
        if (result.status >= VI_SUCCESS) {
            *return_count = result.count;
        }
        return result.status;
    }

    ViStatus flush(ViUInt16 mask) override {
        constexpr ViUInt16 kKnownMask = VI_READ_BUF | VI_WRITE_BUF |
                                        VI_READ_BUF_DISCARD | VI_WRITE_BUF_DISCARD |
                                        VI_IO_IN_BUF | VI_IO_OUT_BUF |
                                        VI_IO_IN_BUF_DISCARD | VI_IO_OUT_BUF_DISCARD;
        if (mask == 0 || (mask & static_cast<ViUInt16>(~kKnownMask)) != 0) {
            return VI_ERROR_INV_MASK;
        }
        return on_stream([this, mask] {
            if ((mask & (VI_READ_BUF_DISCARD | VI_IO_IN_BUF_DISCARD)) != 0) {
                read_ahead_.clear();
            }
            return VI_SUCCESS;
        });
    }

    ViStatus read_stb(ViPUInt16 status) override {
        if (status == nullptr) {
            return VI_ERROR_INV_PARAMETER;
        }
        return VI_ERROR_NSUP_OPER;
    }

    void close() noexcept final {
        try {
            static_cast<void>(on_stream([this] {
                asio::error_code ignored;
                stream_.cancel(ignored);
                stream_.close(ignored);
                return VI_SUCCESS;
            }));
        } catch (...) {
        }
    }

    void notify_cancel() noexcept final {
        // Each operation installs its own cancellation handler. This method is
        // retained for backends such as the condition-variable mock.
    }

protected:
    Stream& stream() noexcept { return stream_; }
    asio::any_io_executor executor() noexcept { return strand_; }

    template <typename Function>
    ViStatus on_stream(Function function) {
        auto completion = std::make_shared<std::promise<ViStatus>>();
        auto future = completion->get_future();
        asio::post(strand_, [completion, function = std::move(function)]() mutable {
            try {
                completion->set_value(function());
            } catch (...) {
                completion->set_exception(std::current_exception());
            }
        });
        return future.get();
    }

private:
    struct ReadResult {
        ViStatus status{VI_ERROR_SYSTEM_ERROR};
        std::vector<ViByte> data;
    };

    struct WriteResult {
        ViStatus status{VI_ERROR_SYSTEM_ERROR};
        ViUInt32 count{0};
    };

    struct RequestBase {
        RequestBase(const asio::strand<asio::any_io_executor>& executor,
                    Operation& value)
            : operation(value), timer(executor) {}

        Operation& operation;
        asio::steady_timer timer;
        asio::cancellation_signal cancellation;
        bool enqueued{false};
        bool active{false};
        bool done{false};
    };

    struct ReadRequest final : RequestBase {
        ReadRequest(const asio::strand<asio::any_io_executor>& executor,
                    Operation& operation, ViUInt32 requested_count,
                    ReadOptions read_options)
            : RequestBase(executor, operation),
              count(requested_count),
              options(read_options) {
            data.reserve(std::min<std::size_t>(requested_count, 4096u));
        }

        ViUInt32 count;
        ReadOptions options;
        std::vector<ViByte> data;
        std::array<ViByte, 4096> scratch{};
        std::promise<ReadResult> completion;
    };

    struct WriteRequest final : RequestBase {
        WriteRequest(const asio::strand<asio::any_io_executor>& executor,
                     Operation& operation, std::vector<ViByte> write_data)
            : RequestBase(executor, operation), data(std::move(write_data)) {}

        std::vector<ViByte> data;
        std::promise<WriteResult> completion;
    };

    static ViStatus map_stream_error(const asio::error_code& error) noexcept {
        if (error == asio::error::eof || error == asio::error::connection_reset ||
            error == asio::error::connection_aborted ||
            error == asio::error::broken_pipe || error == asio::error::not_connected) {
            return VI_ERROR_CONN_LOST;
        }
        if (error == asio::error::access_denied) {
            return VI_ERROR_NPERMISSION;
        }
        return VI_ERROR_IO;
    }

    void arm_timeout(const std::shared_ptr<RequestBase>& request) {
        const auto deadline = request->operation.deadline();
        if (deadline == Operation::Clock::time_point::max()) {
            return;
        }
        request->timer.expires_at(deadline);
        request->timer.async_wait(asio::bind_executor(
            strand_, [request](const asio::error_code& error) {
                if (!error && !request->done) {
                    static_cast<void>(request->operation.request_timeout());
                }
            }));
    }

    void enqueue_read(const std::shared_ptr<ReadRequest>& request) {
        if (request->done) {
            return;
        }
        arm_timeout(request);
        request->enqueued = true;
        read_queue_.push_back(request);
        if (read_queue_.size() == 1u) {
            start_next_read();
        }
    }

    void start_next_read() {
        while (!read_queue_.empty()) {
            const auto request = read_queue_.front();
            if (request->done) {
                read_queue_.pop_front();
                continue;
            }
            if (request->operation.completed()) {
                read_queue_.pop_front();
                fulfill_read(request, request->operation.result(), {});
                continue;
            }
            if (request->operation.deadline() != Operation::Clock::time_point::max() &&
                request->operation.deadline() <= Operation::Clock::now()) {
                static_cast<void>(request->operation.try_complete(VI_ERROR_TMO));
                read_queue_.pop_front();
                fulfill_read(request, request->operation.result(), {});
                continue;
            }
            if (terminal_status_ < VI_SUCCESS && read_ahead_.empty()) {
                static_cast<void>(request->operation.try_complete(terminal_status_));
                read_queue_.pop_front();
                fulfill_read(request, request->operation.result(), {});
                continue;
            }
            request->active = true;
            consume_read_ahead(request);
            return;
        }
    }

    void consume_read_ahead(const std::shared_ptr<ReadRequest>& request) {
        while (!read_ahead_.empty()) {
            const auto byte = read_ahead_.front();
            read_ahead_.pop_front();
            request->data.push_back(byte);
            if (request->options.termchar_enabled && byte == request->options.termchar) {
                complete_active_read(request, VI_SUCCESS_TERM_CHAR);
                return;
            }
            if (request->data.size() == static_cast<std::size_t>(request->count)) {
                complete_active_read(request, VI_SUCCESS_MAX_CNT);
                return;
            }
        }
        if (terminal_status_ < VI_SUCCESS) {
            complete_active_read(request,
                                 request->data.empty() ? terminal_status_ : VI_SUCCESS);
            return;
        }
        issue_read(request);
    }

    void issue_read(const std::shared_ptr<ReadRequest>& request) {
        stream_.async_read_some(
            asio::buffer(request->scratch),
            asio::bind_cancellation_slot(
                request->cancellation.slot(),
                asio::bind_executor(
                    strand_, [this, request](const asio::error_code& error,
                                             std::size_t amount) {
                        std::size_t consumed = 0;
                        for (; consumed < amount; ++consumed) {
                            const auto byte = request->scratch[consumed];
                            request->data.push_back(byte);
                            if ((request->options.termchar_enabled &&
                                 byte == request->options.termchar) ||
                                request->data.size() ==
                                    static_cast<std::size_t>(request->count)) {
                                ++consumed;
                                break;
                            }
                        }
                        for (std::size_t index = consumed; index < amount; ++index) {
                            read_ahead_.push_back(request->scratch[index]);
                        }

                        if (error) {
                            if (error == asio::error::operation_aborted &&
                                request->operation.completed()) {
                                restore_partial_read(request);
                                complete_active_read(request,
                                                     request->operation.result(), false);
                                return;
                            }
                            if (!request->data.empty()) {
                                terminal_status_ = map_stream_error(error);
                                if (request->options.termchar_enabled &&
                                    request->data.back() == request->options.termchar) {
                                    complete_active_read(request, VI_SUCCESS_TERM_CHAR);
                                } else if (request->data.size() ==
                                           static_cast<std::size_t>(request->count)) {
                                    complete_active_read(request, VI_SUCCESS_MAX_CNT);
                                } else {
                                    complete_active_read(request, VI_SUCCESS);
                                }
                            } else {
                                complete_active_read(request, map_stream_error(error));
                            }
                            return;
                        }

                        if (request->options.termchar_enabled && !request->data.empty() &&
                            request->data.back() == request->options.termchar) {
                            complete_active_read(request, VI_SUCCESS_TERM_CHAR);
                        } else if (request->data.size() ==
                                   static_cast<std::size_t>(request->count)) {
                            complete_active_read(request, VI_SUCCESS_MAX_CNT);
                        } else if (amount == 0) {
                            issue_read(request);
                        } else {
                            issue_read(request);
                        }
                    })));
    }

    void restore_partial_read(const std::shared_ptr<ReadRequest>& request) {
        for (auto iterator = request->data.rbegin(); iterator != request->data.rend();
             ++iterator) {
            read_ahead_.push_front(*iterator);
        }
        request->data.clear();
    }

    void complete_active_read(const std::shared_ptr<ReadRequest>& request,
                              ViStatus proposed, bool claim_completion = true) {
        ViStatus status = proposed;
        if (claim_completion && !request->operation.try_complete(proposed)) {
            restore_partial_read(request);
            status = request->operation.result();
        }
        request->timer.cancel();
        request->active = false;
        if (!read_queue_.empty() && read_queue_.front() == request) {
            read_queue_.pop_front();
        }
        auto data = status >= VI_SUCCESS ? std::move(request->data) : std::vector<ViByte>{};
        fulfill_read(request, status, std::move(data));
        start_next_read();
    }

    void fulfill_read(const std::shared_ptr<ReadRequest>& request, ViStatus status,
                      std::vector<ViByte> data) {
        if (request->done) {
            return;
        }
        request->done = true;
        request->timer.cancel();
        request->completion.set_value(ReadResult{status, std::move(data)});
    }

    void cancel_read(const std::shared_ptr<ReadRequest>& request) {
        if (request->done) {
            return;
        }
        if (request->active) {
            request->cancellation.emit(asio::cancellation_type::terminal);
            return;
        }
        if (request->enqueued) {
            const auto found = std::find(read_queue_.begin(), read_queue_.end(), request);
            if (found != read_queue_.end()) {
                read_queue_.erase(found);
            }
        }
        fulfill_read(request, request->operation.result(), {});
    }

    void enqueue_write(const std::shared_ptr<WriteRequest>& request) {
        if (request->done) {
            return;
        }
        arm_timeout(request);
        request->enqueued = true;
        write_queue_.push_back(request);
        if (write_queue_.size() == 1u) {
            start_next_write();
        }
    }

    void start_next_write() {
        while (!write_queue_.empty()) {
            const auto request = write_queue_.front();
            if (request->done) {
                write_queue_.pop_front();
                continue;
            }
            if (request->operation.completed()) {
                write_queue_.pop_front();
                fulfill_write(request, request->operation.result(), 0);
                continue;
            }
            if (request->operation.deadline() != Operation::Clock::time_point::max() &&
                request->operation.deadline() <= Operation::Clock::now()) {
                static_cast<void>(request->operation.try_complete(VI_ERROR_TMO));
                write_queue_.pop_front();
                fulfill_write(request, request->operation.result(), 0);
                continue;
            }
            request->active = true;
            asio::async_write(
                stream_, asio::buffer(request->data),
                asio::bind_cancellation_slot(
                    request->cancellation.slot(),
                    asio::bind_executor(
                        strand_, [this, request](const asio::error_code& error,
                                                 std::size_t amount) {
                            ViStatus status = VI_SUCCESS;
                            ViUInt32 count = 0;
                            if (error) {
                                status = error == asio::error::operation_aborted &&
                                                 request->operation.completed()
                                             ? request->operation.result()
                                             : map_stream_error(error);
                            } else if (request->operation.try_complete(VI_SUCCESS)) {
                                count = static_cast<ViUInt32>(amount);
                            } else {
                                status = request->operation.result();
                            }
                            request->timer.cancel();
                            request->active = false;
                            if (!write_queue_.empty() &&
                                write_queue_.front() == request) {
                                write_queue_.pop_front();
                            }
                            fulfill_write(request, status, count);
                            start_next_write();
                        })));
            return;
        }
    }

    void fulfill_write(const std::shared_ptr<WriteRequest>& request, ViStatus status,
                       ViUInt32 count) {
        if (request->done) {
            return;
        }
        request->done = true;
        request->timer.cancel();
        request->completion.set_value(WriteResult{status, count});
    }

    void cancel_write(const std::shared_ptr<WriteRequest>& request) {
        if (request->done) {
            return;
        }
        if (request->active) {
            request->cancellation.emit(asio::cancellation_type::terminal);
            return;
        }
        if (request->enqueued) {
            const auto found = std::find(write_queue_.begin(), write_queue_.end(), request);
            if (found != write_queue_.end()) {
                write_queue_.erase(found);
            }
        }
        fulfill_write(request, request->operation.result(), 0);
    }

    std::shared_ptr<IoRuntime> runtime_;
    asio::strand<asio::any_io_executor> strand_;
    Stream stream_;
    std::deque<std::shared_ptr<ReadRequest>> read_queue_;
    std::deque<std::shared_ptr<WriteRequest>> write_queue_;
    std::deque<ViByte> read_ahead_;
    ViStatus terminal_status_{VI_SUCCESS};
};

}  // namespace wrvisa

#endif
