#include "backends/asio/request_channel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <future>
#include <limits>
#include <utility>

#include <asio.hpp>

#include "runtime/io_runtime.h"

namespace wrvisa {
namespace {

std::uint32_t read_u32(const std::uint8_t* input) noexcept {
    return (static_cast<std::uint32_t>(input[0]) << 24u) |
           (static_cast<std::uint32_t>(input[1]) << 16u) |
           (static_cast<std::uint32_t>(input[2]) << 8u) |
           static_cast<std::uint32_t>(input[3]);
}

std::uint64_t read_u64(const std::uint8_t* input) noexcept {
    return (static_cast<std::uint64_t>(read_u32(input)) << 32u) |
           static_cast<std::uint64_t>(read_u32(input + 4));
}

ViStatus map_socket_error(const asio::error_code& error) noexcept {
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

ViStatus map_connect_error(const asio::error_code& error) noexcept {
    if (error == asio::error::access_denied) {
        return VI_ERROR_NPERMISSION;
    }
    return VI_ERROR_RSRC_NFOUND;
}

struct ExchangeResult {
    ViStatus status{VI_ERROR_SYSTEM_ERROR};
    std::vector<std::uint8_t> response;
};

}  // namespace

struct RequestChannel::State final : std::enable_shared_from_this<State> {
    struct Request final {
        Request(asio::any_io_executor executor, Operation& value,
                std::vector<std::uint8_t> bytes, ResponseFraming response_framing)
            : operation(value), request(std::move(bytes)), framing(response_framing),
              timer(executor) {}

        Operation& operation;
        std::vector<std::uint8_t> request;
        ResponseFraming framing;
        asio::steady_timer timer;
        asio::cancellation_signal cancellation;
        std::array<std::uint8_t, 16> header{};
        std::vector<std::uint8_t> fragment;
        std::vector<std::uint8_t> response;
        std::promise<ExchangeResult> completion;
        bool enqueued{false};
        bool active{false};
        bool done{false};
    };

    State(std::shared_ptr<IoRuntime> io_runtime, std::size_t frame_limit,
          bool should_drain_on_cancel)
        : runtime(std::move(io_runtime)),
          strand(asio::make_strand(runtime->executor())), resolver(strand), socket(strand),
          maximum_frame_size(frame_limit), drain_on_cancel(should_drain_on_cancel) {}

    std::shared_ptr<IoRuntime> runtime;
    asio::strand<asio::any_io_executor> strand;
    asio::ip::tcp::resolver resolver;
    asio::ip::tcp::socket socket;
    std::deque<std::shared_ptr<Request>> queue;
    std::size_t maximum_frame_size;
    bool drain_on_cancel;
    std::function<void()> cancel_observer;
    bool closed{false};

    void enqueue(const std::shared_ptr<Request>& request) {
        if (request->done) {
            return;
        }
        if (closed || !socket.is_open()) {
            finish(request, VI_ERROR_CONN_LOST);
            return;
        }
        if (request->operation.completed()) {
            finish(request, request->operation.result());
            return;
        }
        request->enqueued = true;
        queue.push_back(request);
        if (queue.size() == 1u) {
            start_next();
        }
    }

    void arm_timer(const std::shared_ptr<Request>& request) {
        const auto deadline = request->operation.deadline();
        if (deadline == Operation::Clock::time_point::max()) {
            return;
        }
        request->timer.expires_at(deadline);
        request->timer.async_wait(asio::bind_executor(
            strand, [request](const asio::error_code& error) {
                if (!error && !request->done) {
                    static_cast<void>(request->operation.request_timeout());
                }
            }));
    }

    void start_next() {
        while (!queue.empty()) {
            const auto request = queue.front();
            if (request->done) {
                queue.pop_front();
                continue;
            }
            if (request->operation.completed()) {
                queue.pop_front();
                finish(request, request->operation.result());
                continue;
            }
            request->active = true;
            arm_timer(request);
            if (request->request.empty()) {
                begin_response(request);
            } else {
                asio::async_write(
                    socket, asio::buffer(request->request),
                    asio::bind_cancellation_slot(
                        request->cancellation.slot(),
                        asio::bind_executor(
                            strand,
                            [self = shared_from_this(), request](
                                const asio::error_code& error, std::size_t) {
                                if (error) {
                                    self->finish_error(request, error);
                                    return;
                                }
                                self->begin_response(request);
                            })));
            }
            return;
        }
    }

    void begin_response(const std::shared_ptr<Request>& request) {
        if (request->operation.completed()) {
            finish(request, request->operation.result());
            return;
        }
        switch (request->framing) {
            case ResponseFraming::none:
                finish(request, VI_SUCCESS);
                return;
            case ResponseFraming::rpc_record:
                read_rpc_marker(request);
                return;
            case ResponseFraming::hislip_frame:
                read_hislip_header(request);
                return;
        }
        finish(request, VI_ERROR_SYSTEM_ERROR);
    }

    void read_rpc_marker(const std::shared_ptr<Request>& request) {
        asio::async_read(
            socket, asio::buffer(request->header.data(), 4u),
            asio::bind_cancellation_slot(
                request->cancellation.slot(),
                asio::bind_executor(
                    strand,
                    [self = shared_from_this(), request](
                        const asio::error_code& error, std::size_t) {
                        if (error) {
                            self->finish_error(request, error);
                            return;
                        }
                        const auto marker = read_u32(request->header.data());
                        const auto size = static_cast<std::size_t>(
                            marker & UINT32_C(0x7FFFFFFF));
                        if (size > self->maximum_frame_size ||
                            request->response.size() >
                                self->maximum_frame_size - size) {
                            self->finish(request, VI_ERROR_IO);
                            return;
                        }
                        request->fragment.assign(size, 0);
                        self->read_rpc_fragment(
                            request, (marker & UINT32_C(0x80000000)) != 0);
                    })));
    }

    void read_rpc_fragment(const std::shared_ptr<Request>& request, bool last) {
        if (request->fragment.empty()) {
            if (last) {
                finish(request, VI_SUCCESS);
            } else {
                read_rpc_marker(request);
            }
            return;
        }
        asio::async_read(
            socket, asio::buffer(request->fragment),
            asio::bind_cancellation_slot(
                request->cancellation.slot(),
                asio::bind_executor(
                    strand,
                    [self = shared_from_this(), request, last](
                        const asio::error_code& error, std::size_t) {
                        if (error) {
                            self->finish_error(request, error);
                            return;
                        }
                        request->response.insert(request->response.end(),
                                                 request->fragment.begin(),
                                                 request->fragment.end());
                        request->fragment.clear();
                        if (last) {
                            self->finish(request, VI_SUCCESS);
                        } else {
                            self->read_rpc_marker(request);
                        }
                    })));
    }

    void read_hislip_header(const std::shared_ptr<Request>& request) {
        asio::async_read(
            socket, asio::buffer(request->header),
            asio::bind_cancellation_slot(
                request->cancellation.slot(),
                asio::bind_executor(
                    strand,
                    [self = shared_from_this(), request](
                        const asio::error_code& error, std::size_t) {
                        if (error) {
                            self->finish_error(request, error);
                            return;
                        }
                        const auto payload_size = read_u64(request->header.data() + 8u);
                        if (request->header[0] != 'H' || request->header[1] != 'S' ||
                            payload_size > self->maximum_frame_size ||
                            payload_size >
                                static_cast<std::uint64_t>(
                                    std::numeric_limits<std::size_t>::max() - 16u)) {
                            self->finish(request, VI_ERROR_IO);
                            return;
                        }
                        request->response.assign(request->header.begin(),
                                                 request->header.end());
                        request->fragment.assign(
                            static_cast<std::size_t>(payload_size), 0);
                        self->read_hislip_payload(request);
                    })));
    }

    void read_hislip_payload(const std::shared_ptr<Request>& request) {
        if (request->fragment.empty()) {
            finish(request, VI_SUCCESS);
            return;
        }
        asio::async_read(
            socket, asio::buffer(request->fragment),
            asio::bind_cancellation_slot(
                request->cancellation.slot(),
                asio::bind_executor(
                    strand,
                    [self = shared_from_this(), request](
                        const asio::error_code& error, std::size_t) {
                        if (error) {
                            self->finish_error(request, error);
                            return;
                        }
                        request->response.insert(request->response.end(),
                                                 request->fragment.begin(),
                                                 request->fragment.end());
                        request->fragment.clear();
                        self->finish(request, VI_SUCCESS);
                    })));
    }

    void finish_error(const std::shared_ptr<Request>& request,
                      const asio::error_code& error) {
        if (error == asio::error::operation_aborted &&
            request->operation.completed()) {
            finish(request, request->operation.result());
            return;
        }
        finish(request, map_socket_error(error));
    }

    void finish(const std::shared_ptr<Request>& request, ViStatus proposed) {
        if (request->done) {
            return;
        }
        ViStatus status = proposed;
        if (request->operation.completed()) {
            status = request->operation.result();
        } else if (status < VI_SUCCESS && !request->operation.try_complete(status)) {
            status = request->operation.result();
        }
        request->done = true;
        request->active = false;
        request->timer.cancel();
        if (!queue.empty() && queue.front() == request) {
            queue.pop_front();
        } else {
            const auto found = std::find(queue.begin(), queue.end(), request);
            if (found != queue.end()) {
                queue.erase(found);
            }
        }
        auto response = status >= VI_SUCCESS ? std::move(request->response)
                                             : std::vector<std::uint8_t>{};
        request->completion.set_value(ExchangeResult{status, std::move(response)});
        if (!closed) {
            start_next();
        }
    }

    void cancel(const std::shared_ptr<Request>& request) {
        if (request->done) {
            return;
        }
        if (request->active) {
            if (drain_on_cancel) {
                request->timer.cancel();
                if (cancel_observer) {
                    try {
                        cancel_observer();
                    } catch (...) {
                    }
                }
                request->timer.expires_after(std::chrono::seconds(1));
                request->timer.async_wait(asio::bind_executor(
                    strand,
                    [self = shared_from_this(), request](
                        const asio::error_code& error) {
                        if (!error && !request->done) {
                            self->close();
                        }
                    }));
                return;
            }
            request->cancellation.emit(asio::cancellation_type::terminal);
            return;
        }
        finish(request, request->operation.result());
    }

    void close() {
        if (closed) {
            return;
        }
        closed = true;
        asio::error_code ignored;
        resolver.cancel();
        socket.cancel(ignored);
        socket.close(ignored);
        const auto pending = queue;
        for (const auto& request : pending) {
            finish(request, request->operation.completed()
                                ? request->operation.result()
                                : VI_ERROR_CONN_LOST);
        }
    }
};

RequestChannel::RequestChannel(std::size_t maximum_frame_size,
                               bool drain_on_cancel)
    : state_(std::make_shared<State>(shared_io_runtime(), maximum_frame_size,
                                     drain_on_cancel)) {}

RequestChannel::~RequestChannel() { close(); }

ViStatus RequestChannel::connect(const std::string& host, std::uint16_t port,
                                 ViUInt32 timeout, std::string& address) {
    struct ConnectResult {
        ViStatus status;
        std::string address;
    };
    struct ConnectState final {
        explicit ConnectState(asio::any_io_executor executor)
            : timer(executor) {}
        asio::steady_timer timer;
        std::promise<ConnectResult> completion;
        bool done{false};
    };

    const auto channel = state_;
    const auto connect = std::make_shared<ConnectState>(channel->strand);
    auto future = connect->completion.get_future();
    asio::post(channel->strand, [channel, connect, host, port, timeout] {
        auto finish = [channel, connect](ViStatus status, std::string value = {}) {
            if (connect->done) {
                return;
            }
            connect->done = true;
            connect->timer.cancel();
            channel->resolver.cancel();
            if (status < VI_SUCCESS) {
                asio::error_code ignored;
                channel->socket.cancel(ignored);
                channel->socket.close(ignored);
            }
            connect->completion.set_value(ConnectResult{status, std::move(value)});
        };
        if (timeout != VI_TMO_INFINITE) {
            connect->timer.expires_after(std::chrono::milliseconds(timeout));
            connect->timer.async_wait(asio::bind_executor(
                channel->strand, [finish](const asio::error_code& error) mutable {
                    if (!error) {
                        finish(VI_ERROR_TMO);
                    }
                }));
        }
        channel->resolver.async_resolve(
            host, std::to_string(port),
            asio::bind_executor(
                channel->strand,
                [channel, finish](
                    const asio::error_code& error,
                    const asio::ip::tcp::resolver::results_type& endpoints) mutable {
                    if (error) {
                        finish(map_connect_error(error));
                        return;
                    }
                    asio::async_connect(
                        channel->socket, endpoints,
                        asio::bind_executor(
                            channel->strand,
                            [finish](const asio::error_code& connect_error,
                                     const asio::ip::tcp::endpoint& endpoint) mutable {
                                finish(connect_error
                                           ? map_connect_error(connect_error)
                                           : VI_SUCCESS,
                                       connect_error
                                           ? std::string{}
                                           : endpoint.address().to_string());
                            }));
                }));
    });
    auto result = future.get();
    address = std::move(result.address);
    return result.status;
}

ViStatus RequestChannel::exchange(Operation& operation,
                                  std::vector<std::uint8_t> request,
                                  ResponseFraming framing,
                                  std::vector<std::uint8_t>& response) {
    response.clear();
    const auto channel = state_;
    auto pending = std::make_shared<State::Request>(
        channel->strand, operation, std::move(request), framing);
    auto future = pending->completion.get_future();
    operation.set_cancel_handler(
        [weak_channel = std::weak_ptr<State>(channel),
         weak_request = std::weak_ptr<State::Request>(pending)] {
            if (const auto locked_channel = weak_channel.lock()) {
                asio::post(locked_channel->strand,
                           [weak_channel, weak_request] {
                               const auto channel_value = weak_channel.lock();
                               const auto request_value = weak_request.lock();
                               if (channel_value && request_value) {
                                   channel_value->cancel(request_value);
                               }
                           });
            }
        });
    try {
        asio::post(channel->strand,
                   [channel, pending] { channel->enqueue(pending); });
    } catch (...) {
        operation.clear_cancel_handler();
        throw;
    }
    auto result = future.get();
    operation.clear_cancel_handler();
    response = std::move(result.response);
    return result.status;
}

void RequestChannel::set_cancel_observer(std::function<void()> observer) {
    const auto channel = state_;
    auto completion = std::make_shared<std::promise<void>>();
    auto future = completion->get_future();
    asio::post(channel->strand,
               [channel, completion, observer = std::move(observer)]() mutable {
                   channel->cancel_observer = std::move(observer);
                   completion->set_value();
               });
    future.get();
}

void RequestChannel::close() noexcept {
    const auto channel = state_;
    if (!channel) {
        return;
    }
    try {
        if (channel->strand.running_in_this_thread()) {
            channel->close();
            return;
        }
        auto completion = std::make_shared<std::promise<void>>();
        auto future = completion->get_future();
        asio::post(channel->strand, [channel, completion] {
            channel->close();
            completion->set_value();
        });
        future.get();
    } catch (...) {
    }
}

}  // namespace wrvisa
