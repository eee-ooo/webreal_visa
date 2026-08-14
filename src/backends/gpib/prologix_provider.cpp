#include "backends/gpib/prologix_provider.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <termios.h>
#endif

#include "platform/serial_path.h"
#include "runtime/io_runtime.h"

namespace wrvisa {
namespace {

constexpr std::uint32_t kReadGraceMs = 250;
constexpr std::size_t kVersionLimit = 256;

ViStatus map_stream_error(const asio::error_code& error) noexcept {
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
    if (error == asio::error::timed_out) {
        return VI_ERROR_TMO;
    }
    return VI_ERROR_RSRC_NFOUND;
}

class PrologixIo {
public:
    virtual ~PrologixIo() = default;
    virtual ViStatus connect(Operation& operation) = 0;
    virtual ViStatus write(Operation& operation,
                           std::span<const std::uint8_t> data) = 0;
    virtual ViStatus read_until(Operation& operation, std::uint8_t delimiter,
                                std::size_t maximum_size,
                                std::uint32_t idle_timeout_ms,
                                std::vector<std::uint8_t>& data) = 0;
    virtual void cancel() noexcept = 0;
    virtual void close() noexcept = 0;
};

template <typename Stream>
class AsioPrologixIo final : public PrologixIo {
public:
    explicit AsioPrologixIo(PrologixConfiguration configuration)
        : configuration_(std::move(configuration)),
          runtime_(shared_io_runtime()),
          strand_(asio::make_strand(runtime_->executor())),
          resolver_(strand_),
          stream_(strand_) {}

    ~AsioPrologixIo() override { close(); }

    ViStatus connect(Operation& operation) override {
        if (operation.completed()) {
            return operation.result();
        }
        if constexpr (std::is_same_v<Stream, asio::serial_port>) {
            return connect_serial(operation);
        } else {
            return connect_tcp(operation);
        }
    }

    ViStatus write(Operation& operation,
                   std::span<const std::uint8_t> data) override {
        if (operation.completed()) {
            return operation.result();
        }
        auto request = std::make_shared<WriteRequest>(
            strand_, operation, std::vector<std::uint8_t>(data.begin(), data.end()));
        auto future = request->completion.get_future();
        operation.set_cancel_handler(
            [strand = strand_, weak = std::weak_ptr<WriteRequest>(request)] {
                asio::post(strand, [weak] {
                    if (const auto locked = weak.lock()) {
                        locked->cancellation.emit(asio::cancellation_type::terminal);
                    }
                });
            });
        try {
            asio::post(strand_, [this, request] { start_write(request); });
        } catch (...) {
            operation.clear_cancel_handler();
            throw;
        }
        const auto status = future.get();
        operation.clear_cancel_handler();
        return status;
    }

    ViStatus read_until(Operation& operation, std::uint8_t delimiter,
                        std::size_t maximum_size,
                        std::uint32_t idle_timeout_ms,
                        std::vector<std::uint8_t>& data) override {
        data.clear();
        if (operation.completed()) {
            return operation.result();
        }
        auto request = std::make_shared<ReadRequest>(
            strand_, operation, delimiter, maximum_size, idle_timeout_ms);
        auto future = request->completion.get_future();
        operation.set_cancel_handler(
            [strand = strand_, weak = std::weak_ptr<ReadRequest>(request)] {
                asio::post(strand, [weak] {
                    if (const auto locked = weak.lock()) {
                        locked->cancellation.emit(asio::cancellation_type::terminal);
                    }
                });
            });
        try {
            asio::post(strand_, [this, request] { start_read(request); });
        } catch (...) {
            operation.clear_cancel_handler();
            throw;
        }
        auto result = future.get();
        operation.clear_cancel_handler();
        if (result.status >= VI_SUCCESS) {
            data = std::move(result.data);
        }
        return result.status;
    }

    void cancel() noexcept override {
        try {
            asio::post(strand_, [this] {
                asio::error_code ignored;
                stream_.cancel(ignored);
            });
        } catch (...) {
        }
    }

    void close() noexcept override {
        try {
            auto completion = std::make_shared<std::promise<void>>();
            auto future = completion->get_future();
            asio::post(strand_, [this, completion] {
                resolver_.cancel();
                asio::error_code ignored;
                stream_.cancel(ignored);
                stream_.close(ignored);
                read_ahead_.clear();
                completion->set_value();
            });
            future.get();
        } catch (...) {
        }
    }

private:
    struct ReadResult {
        ViStatus status{VI_ERROR_SYSTEM_ERROR};
        std::vector<std::uint8_t> data;
    };

    struct WriteRequest {
        WriteRequest(asio::any_io_executor executor, Operation& value,
                     std::vector<std::uint8_t> bytes)
            : operation(value), data(std::move(bytes)), timer(executor) {}

        Operation& operation;
        std::vector<std::uint8_t> data;
        asio::steady_timer timer;
        asio::cancellation_signal cancellation;
        std::promise<ViStatus> completion;
        bool done{false};
    };

    struct ReadRequest {
        ReadRequest(asio::any_io_executor executor, Operation& value,
                    std::uint8_t marker, std::size_t limit,
                    std::uint32_t idle_ms)
            : operation(value), delimiter(marker), maximum_size(limit),
              idle_timeout_ms(idle_ms), timer(executor) {
            data.reserve(std::min<std::size_t>(limit, 4096u));
        }

        Operation& operation;
        std::uint8_t delimiter;
        std::size_t maximum_size;
        std::uint32_t idle_timeout_ms;
        asio::steady_timer timer;
        asio::cancellation_signal cancellation;
        std::array<std::uint8_t, 4096> scratch{};
        std::vector<std::uint8_t> data;
        std::promise<ReadResult> completion;
        bool done{false};
    };

    struct ConnectResult {
        ViStatus status{VI_ERROR_SYSTEM_ERROR};
    };

    void arm_absolute_timer(const std::shared_ptr<WriteRequest>& request) {
        if (request->operation.deadline() == Operation::Clock::time_point::max()) {
            return;
        }
        request->timer.expires_at(request->operation.deadline());
        request->timer.async_wait(asio::bind_executor(
            strand_, [request](const asio::error_code& error) {
                if (!error && !request->done) {
                    static_cast<void>(request->operation.request_timeout());
                }
            }));
    }

    void start_write(const std::shared_ptr<WriteRequest>& request) {
        if (request->operation.completed()) {
            finish_write(request, request->operation.result());
            return;
        }
        arm_absolute_timer(request);
        asio::async_write(
            stream_, asio::buffer(request->data),
            asio::bind_cancellation_slot(
                request->cancellation.slot(),
                asio::bind_executor(
                    strand_, [this, request](const asio::error_code& error,
                                             std::size_t amount) {
                        static_cast<void>(this);
                        ViStatus status = VI_SUCCESS;
                        if (error) {
                            status = error == asio::error::operation_aborted &&
                                             request->operation.completed()
                                         ? request->operation.result()
                                         : map_stream_error(error);
                        } else if (amount != request->data.size()) {
                            status = VI_ERROR_IO;
                        } else if (request->operation.completed()) {
                            status = request->operation.result();
                        }
                        finish_write(request, status);
                    })));
    }

    static void finish_write(const std::shared_ptr<WriteRequest>& request,
                             ViStatus status) {
        if (request->done) {
            return;
        }
        request->done = true;
        request->timer.cancel();
        request->completion.set_value(status);
    }

    void arm_read_timer(const std::shared_ptr<ReadRequest>& request) {
        auto deadline = Operation::Clock::now() +
                        std::chrono::milliseconds(
                            request->idle_timeout_ms + kReadGraceMs);
        deadline = std::min(deadline, request->operation.deadline());
        request->timer.expires_at(deadline);
        request->timer.async_wait(asio::bind_executor(
            strand_, [request](const asio::error_code& error) {
                if (!error && !request->done) {
                    static_cast<void>(request->operation.request_timeout());
                }
            }));
    }

    void start_read(const std::shared_ptr<ReadRequest>& request) {
        if (request->operation.completed()) {
            finish_read(request, request->operation.result());
            return;
        }
        arm_read_timer(request);
        consume_read_ahead(request);
    }

    void consume_read_ahead(const std::shared_ptr<ReadRequest>& request) {
        while (!read_ahead_.empty() && !request->done) {
            const auto value = read_ahead_.front();
            read_ahead_.pop_front();
            if (value == request->delimiter) {
                finish_read(request, VI_SUCCESS);
                return;
            }
            if (request->data.size() >= request->maximum_size) {
                finish_read(request, VI_ERROR_IO);
                return;
            }
            request->data.push_back(value);
        }
        if (!request->done) {
            issue_read(request);
        }
    }

    void issue_read(const std::shared_ptr<ReadRequest>& request) {
        if (request->operation.completed()) {
            finish_read(request, request->operation.result());
            return;
        }
        stream_.async_read_some(
            asio::buffer(request->scratch),
            asio::bind_cancellation_slot(
                request->cancellation.slot(),
                asio::bind_executor(
                    strand_, [this, request](const asio::error_code& error,
                                             std::size_t amount) {
                        for (std::size_t index = 0; index < amount; ++index) {
                            const auto value = request->scratch[index];
                            if (value == request->delimiter) {
                                if (index + 1u != amount) {
                                    finish_read(request, VI_ERROR_IO);
                                } else {
                                    finish_read(request, VI_SUCCESS);
                                }
                                return;
                            }
                            if (request->data.size() >= request->maximum_size) {
                                finish_read(request, VI_ERROR_IO);
                                return;
                            }
                            request->data.push_back(value);
                        }
                        if (error) {
                            const auto status =
                                error == asio::error::operation_aborted &&
                                        request->operation.completed()
                                    ? request->operation.result()
                                    : map_stream_error(error);
                            finish_read(request, status);
                            return;
                        }
                        if (request->operation.completed()) {
                            finish_read(request, request->operation.result());
                            return;
                        }
                        arm_read_timer(request);
                        issue_read(request);
                    })));
    }

    static void finish_read(const std::shared_ptr<ReadRequest>& request,
                            ViStatus status) {
        if (request->done) {
            return;
        }
        request->done = true;
        request->timer.cancel();
        auto data = status >= VI_SUCCESS ? std::move(request->data)
                                         : std::vector<std::uint8_t>{};
        request->completion.set_value(ReadResult{status, std::move(data)});
    }

    ViStatus connect_serial(Operation& operation) {
        auto completion = std::make_shared<std::promise<ViStatus>>();
        auto future = completion->get_future();
        asio::post(strand_, [this, completion, &operation] {
            if (operation.completed()) {
                completion->set_value(operation.result());
                return;
            }
            asio::error_code error;
            stream_.open(platform_serial_path(configuration_.endpoint), error);
            const bool opened = !error;
            if (!error) {
                stream_.set_option(asio::serial_port_base::baud_rate(9600), error);
            }
            if (!error) {
                stream_.set_option(
                    asio::serial_port_base::character_size(8), error);
            }
            if (!error) {
                stream_.set_option(asio::serial_port_base::parity(
                                       asio::serial_port_base::parity::none),
                                   error);
            }
            if (!error) {
                stream_.set_option(asio::serial_port_base::stop_bits(
                                       asio::serial_port_base::stop_bits::one),
                                   error);
            }
            if (!error) {
                stream_.set_option(asio::serial_port_base::flow_control(
                                       asio::serial_port_base::flow_control::none),
                                   error);
            }
            if (!error) {
#if defined(_WIN32)
                if (PurgeComm(stream_.native_handle(),
                              PURGE_RXCLEAR | PURGE_TXCLEAR) == 0) {
                    error = asio::error::fault;
                }
#else
                if (tcflush(stream_.native_handle(), TCIOFLUSH) != 0) {
                    error = asio::error::fault;
                }
#endif
            }
            if (!operation.completed() &&
                operation.deadline() != Operation::Clock::time_point::max() &&
                operation.deadline() <= Operation::Clock::now()) {
                static_cast<void>(operation.request_timeout());
            }

            if (error || operation.completed()) {
                asio::error_code ignored;
                stream_.close(ignored);
            }
            completion->set_value(
                operation.completed()
                    ? operation.result()
                    : (error ? (opened ? VI_ERROR_IO : map_connect_error(error))
                             : VI_SUCCESS));
        });
        return future.get();
    }

    ViStatus connect_tcp(Operation& operation) {
        struct ConnectState {
            explicit ConnectState(asio::any_io_executor executor)
                : timer(executor) {}
            asio::steady_timer timer;
            std::promise<ViStatus> completion;
            bool done{false};
        };
        auto state = std::make_shared<ConnectState>(strand_);
        auto future = state->completion.get_future();
        operation.set_cancel_handler([this] {
            asio::post(strand_, [this] {
                resolver_.cancel();
                asio::error_code ignored;
                stream_.cancel(ignored);
                stream_.close(ignored);
            });
        });
        asio::post(strand_, [this, state, &operation] {
            auto finish = [this, state, &operation](ViStatus status) {
                if (state->done) {
                    return;
                }
                state->done = true;
                state->timer.cancel();
                if (operation.completed()) {
                    status = operation.result();
                }
                if (status < VI_SUCCESS) {
                    resolver_.cancel();
                    asio::error_code ignored;
                    stream_.cancel(ignored);
                    stream_.close(ignored);
                }
                state->completion.set_value(status);
            };
            if (operation.completed()) {
                finish(operation.result());
                return;
            }
            if (operation.deadline() != Operation::Clock::time_point::max()) {
                state->timer.expires_at(operation.deadline());
                state->timer.async_wait(asio::bind_executor(
                    strand_, [finish, &operation](const asio::error_code& error) {
                        if (!error) {
                            static_cast<void>(operation.request_timeout());
                            finish(operation.result());
                        }
                    }));
            }
            resolver_.async_resolve(
                configuration_.endpoint,
                std::to_string(configuration_.tcp_port),
                asio::bind_executor(
                    strand_, [this, finish](
                                 const asio::error_code& error,
                                 const asio::ip::tcp::resolver::results_type& endpoints) {
                        if (error) {
                            finish(map_connect_error(error));
                            return;
                        }
                        asio::async_connect(
                            stream_, endpoints,
                            asio::bind_executor(
                                strand_, [finish](
                                             const asio::error_code& connect_error,
                                             const asio::ip::tcp::endpoint&) {
                                    finish(connect_error
                                               ? map_connect_error(connect_error)
                                               : VI_SUCCESS);
                                }));
                    }));
        });
        const auto status = future.get();
        operation.clear_cancel_handler();
        return status;
    }

    PrologixConfiguration configuration_;
    std::shared_ptr<IoRuntime> runtime_;
    asio::strand<asio::any_io_executor> strand_;
    asio::ip::tcp::resolver resolver_;
    Stream stream_;
    std::deque<std::uint8_t> read_ahead_;
};

std::unique_ptr<PrologixIo> make_io(
    const PrologixConfiguration& configuration) {
    if (configuration.connection == PrologixConnectionKind::serial) {
        return std::make_unique<AsioPrologixIo<asio::serial_port>>(configuration);
    }
    return std::make_unique<AsioPrologixIo<asio::ip::tcp::socket>>(configuration);
}

std::vector<std::uint8_t> ascii_bytes(std::string_view value) {
    return {value.begin(), value.end()};
}

std::string address_suffix(const ResourceDescriptor& resource) {
    std::string result = std::to_string(resource.gpib_primary_address);
    if (resource.gpib_has_secondary_address) {
        result += " " +
                  std::to_string(96u + resource.gpib_secondary_address);
    }
    return result;
}

bool same_configuration(const PrologixConfiguration& left,
                        const PrologixConfiguration& right) {
    return left.connection == right.connection &&
           left.endpoint == right.endpoint && left.tcp_port == right.tcp_port &&
           left.eot_char == right.eot_char &&
           left.read_timeout_ms == right.read_timeout_ms &&
           left.maximum_response_size == right.maximum_response_size;
}

class PrologixController final
    : public std::enable_shared_from_this<PrologixController> {
public:
    explicit PrologixController(PrologixConfiguration configuration)
        : configuration_(std::move(configuration)), io_(make_io(configuration_)) {}

    ~PrologixController() { io_->close(); }

    const PrologixConfiguration& configuration() const noexcept {
        return configuration_;
    }

    std::uint64_t allocate_owner() noexcept {
        return next_owner_.fetch_add(1, std::memory_order_relaxed);
    }

    ViStatus open(std::uint64_t owner, Operation& operation) {
        return transact(owner, operation, [](PrologixController&, Operation&) {
            return VI_SUCCESS;
        });
    }

    ViStatus read(std::uint64_t owner, Operation& operation,
                  const ResourceDescriptor& resource,
                  std::vector<std::uint8_t>& response) {
        return transact(owner, operation,
                        [&resource, &response](PrologixController& self,
                                               Operation& active) {
                            auto status = self.select_address(active, resource);
                            if (status < VI_SUCCESS) {
                                return status;
                            }
                            status = self.write_command(active, "++read eoi\n");
                            if (status < VI_SUCCESS) {
                                return status;
                            }
                            return self.io_->read_until(
                                active, self.configuration_.eot_char,
                                self.configuration_.maximum_response_size,
                                self.configuration_.read_timeout_ms, response);
                        });
    }

    ViStatus write(std::uint64_t owner, Operation& operation,
                   const ResourceDescriptor& resource,
                   std::span<const std::uint8_t> data, bool send_end) {
        return transact(owner, operation,
                        [&resource, data, send_end](PrologixController& self,
                                                   Operation& active) {
                            auto status = self.select_address(active, resource);
                            if (status < VI_SUCCESS) {
                                return status;
                            }
                            if (!self.current_send_end_ ||
                                *self.current_send_end_ != send_end) {
                                status = self.write_command(
                                    active, send_end ? "++eoi 1\n" : "++eoi 0\n");
                                if (status < VI_SUCCESS) {
                                    return status;
                                }
                                self.current_send_end_ = send_end;
                            }
                            std::vector<std::uint8_t> encoded;
                            if (data.size() >
                                (encoded.max_size() - 1u) / 2u) {
                                return VI_ERROR_INV_SIZE;
                            }
                            encoded.reserve(data.size() * 2u + 1u);
                            for (const auto value : data) {
                                if (value == '\r' || value == '\n' ||
                                    value == 0x1bu || value == '+') {
                                    encoded.push_back(0x1bu);
                                }
                                encoded.push_back(value);
                            }
                            encoded.push_back('\n');
                            return self.io_->write(active, encoded);
                        });
    }

    ViStatus clear(std::uint64_t owner, Operation& operation,
                   const ResourceDescriptor& resource) {
        return transact(owner, operation,
                        [&resource](PrologixController& self, Operation& active) {
                            auto status = self.select_address(active, resource);
                            return status < VI_SUCCESS
                                       ? status
                                       : self.write_command(active, "++clr\n");
                        });
    }

    ViStatus trigger(std::uint64_t owner, Operation& operation,
                     const ResourceDescriptor& resource) {
        return transact(owner, operation,
                        [&resource](PrologixController& self, Operation& active) {
                            return self.write_command(
                                active, "++trg " + address_suffix(resource) + "\n");
                        });
    }

    ViStatus serial_poll(std::uint64_t owner, Operation& operation,
                         const ResourceDescriptor& resource,
                         std::uint8_t& status_byte) {
        return transact(owner, operation,
                        [&resource, &status_byte](PrologixController& self,
                                                 Operation& active) {
                            auto status = self.write_command(
                                active, "++spoll " + address_suffix(resource) + "\n");
                            if (status < VI_SUCCESS) {
                                return status;
                            }
                            std::vector<std::uint8_t> line;
                            status = self.io_->read_until(
                                active, '\n', 8u,
                                self.configuration_.read_timeout_ms, line);
                            if (status < VI_SUCCESS) {
                                return status;
                            }
                            if (!line.empty() && line.back() == '\r') {
                                line.pop_back();
                            }
                            unsigned int value = 0;
                            const auto* first = reinterpret_cast<const char*>(line.data());
                            const auto* last = first + line.size();
                            const auto parsed = std::from_chars(first, last, value);
                            if (line.empty() || parsed.ec != std::errc{} ||
                                parsed.ptr != last || value > 255u) {
                                return VI_ERROR_IO;
                            }
                            status_byte = static_cast<std::uint8_t>(value);
                            return VI_SUCCESS;
                        });
    }

    void cancel(std::uint64_t owner) noexcept {
        bool active = false;
        {
            std::lock_guard lock(gate_mutex_);
            active = active_owner_ == owner;
            gate_condition_.notify_all();
        }
        if (active) {
            reconnect_required_.store(true, std::memory_order_release);
            io_->cancel();
        }
    }

private:
    class Lease {
    public:
        Lease(PrologixController& controller, std::uint64_t owner)
            : controller_(controller), owner_(owner) {}
        ~Lease() { controller_.release(owner_); }

    private:
        PrologixController& controller_;
        std::uint64_t owner_;
    };

    bool acquire(std::uint64_t owner, Operation& operation) {
        std::unique_lock lock(gate_mutex_);
        if (operation.deadline() != Operation::Clock::time_point::max() &&
            operation.deadline() <= Operation::Clock::now()) {
            static_cast<void>(operation.request_timeout());
            return false;
        }
        const auto ready = [&] {
            return active_owner_ == 0 || operation.completed();
        };

        if (operation.deadline() == Operation::Clock::time_point::max()) {
            gate_condition_.wait(lock, ready);
        } else if (!gate_condition_.wait_until(lock, operation.deadline(), ready)) {
            static_cast<void>(operation.request_timeout());
        }
        if (operation.completed()) {
            return false;
        }
        active_owner_ = owner;
        return true;
    }

    void release(std::uint64_t owner) noexcept {
        std::lock_guard lock(gate_mutex_);
        if (active_owner_ == owner) {
            active_owner_ = 0;
        }
        gate_condition_.notify_all();
    }

    template <typename Function>
    ViStatus transact(std::uint64_t owner, Operation& operation,
                      Function&& function) {
        if (!acquire(owner, operation)) {
            return operation.result();
        }
        Lease lease(*this, owner);
        if (reconnect_required_.load(std::memory_order_acquire)) {
            invalidate();
        }
        auto status = ensure_ready(operation);
        if (status >= VI_SUCCESS) {
            status = function(*this, operation);
        }
        if (status < VI_SUCCESS ||
            reconnect_required_.load(std::memory_order_acquire)) {
            invalidate();
        }
        return operation.completed() ? operation.result() : status;
    }

    ViStatus ensure_ready(Operation& operation) {
        if (initialized_) {
            return VI_SUCCESS;
        }
        auto status = io_->connect(operation);
        if (status < VI_SUCCESS) {
            return status;
        }
        std::string initialization =
            "++savecfg 0\n++mode 1\n++auto 0\n++eos 3\n++eot_char ";
        initialization += std::to_string(configuration_.eot_char);
        initialization += "\n++eot_enable 1\n++read_tmo_ms ";
        initialization += std::to_string(configuration_.read_timeout_ms);
        initialization += "\n++ver\n";
        status = io_->write(operation, ascii_bytes(initialization));
        if (status < VI_SUCCESS) {
            return status;
        }
        std::vector<std::uint8_t> version;
        status = io_->read_until(operation, '\n', kVersionLimit,
                                 configuration_.read_timeout_ms, version);
        if (status < VI_SUCCESS) {
            return status;
        }
        if (!version.empty() && version.back() == '\r') {
            version.pop_back();
        }
        const std::string text(version.begin(), version.end());
        if (text.find("Prologix") == std::string::npos ||
            text.find("GPIB") == std::string::npos) {
            return VI_ERROR_IO;
        }
        current_address_.reset();
        current_send_end_.reset();
        initialized_ = true;
        reconnect_required_.store(false, std::memory_order_release);
        return VI_SUCCESS;
    }

    void invalidate() noexcept {
        initialized_ = false;
        current_address_.reset();
        current_send_end_.reset();
        io_->close();
        reconnect_required_.store(false, std::memory_order_release);
    }

    ViStatus write_command(Operation& operation, const std::string& command) {
        return io_->write(operation, ascii_bytes(command));
    }

    ViStatus select_address(Operation& operation,
                            const ResourceDescriptor& resource) {
        const auto address = address_suffix(resource);
        if (current_address_ && *current_address_ == address) {
            return VI_SUCCESS;
        }
        const auto status = write_command(operation, "++addr " + address + "\n");
        if (status >= VI_SUCCESS) {
            current_address_ = address;
        }
        return status;
    }

    PrologixConfiguration configuration_;
    std::unique_ptr<PrologixIo> io_;
    std::atomic<std::uint64_t> next_owner_{1};
    std::mutex gate_mutex_;
    std::condition_variable gate_condition_;
    std::uint64_t active_owner_{0};
    std::atomic<bool> reconnect_required_{false};
    bool initialized_{false};
    std::optional<std::string> current_address_;
    std::optional<bool> current_send_end_;
};

class PrologixTransport final : public GpibTransport {
public:
    PrologixTransport(std::shared_ptr<PrologixController> controller,
                      ResourceDescriptor resource, std::uint64_t owner)
        : controller_(std::move(controller)), resource_(std::move(resource)),
          owner_(owner) {}

    ~PrologixTransport() override { close(); }

    GpibCapabilities capabilities() const noexcept override {
        return {true, true, true, true};
    }

    ViStatus read(Operation& operation, std::size_t maximum_size,
                  std::vector<std::uint8_t>& data, bool& end) override {
        data.clear();
        end = false;
        if (closed_.load(std::memory_order_acquire)) {
            return VI_ERROR_CONN_LOST;
        }
        if (operation.completed()) {
            return operation.result();
        }
        if (response_.empty()) {
            std::vector<std::uint8_t> response;
            const auto status = controller_->read(owner_, operation, resource_, response);
            if (status < VI_SUCCESS) {
                return status;
            }
            response_.insert(response_.end(), response.begin(), response.end());
            response_end_ = true;
        }
        const auto amount = std::min(maximum_size, response_.size());
        data.reserve(amount);
        for (std::size_t index = 0; index < amount; ++index) {
            data.push_back(response_.front());
            response_.pop_front();
        }
        end = response_.empty() && response_end_;
        if (end) {
            response_end_ = false;
        }
        return VI_SUCCESS;
    }

    ViStatus write(Operation& operation, std::span<const std::uint8_t> data,
                   bool send_end, std::size_t& transferred) override {
        transferred = 0;
        if (closed_.load(std::memory_order_acquire)) {
            return VI_ERROR_CONN_LOST;
        }
        const auto status =
            controller_->write(owner_, operation, resource_, data, send_end);
        if (status >= VI_SUCCESS) {
            transferred = data.size();
        }
        return status;
    }

    ViStatus clear(Operation& operation) override {
        if (closed_.load(std::memory_order_acquire)) {
            return VI_ERROR_CONN_LOST;
        }
        const auto status = controller_->clear(owner_, operation, resource_);
        if (status >= VI_SUCCESS) {
            response_.clear();
            response_end_ = false;
        }
        return status;
    }

    ViStatus trigger(Operation& operation) override {
        return closed_.load(std::memory_order_acquire)
                   ? VI_ERROR_CONN_LOST
                   : controller_->trigger(owner_, operation, resource_);
    }

    ViStatus serial_poll(Operation& operation,
                         std::uint8_t& status_byte) override {
        return closed_.load(std::memory_order_acquire)
                   ? VI_ERROR_CONN_LOST
                   : controller_->serial_poll(owner_, operation, resource_,
                                              status_byte);
    }

    void discard_read_buffer() noexcept override {
        response_.clear();
        response_end_ = false;
    }

    void cancel() noexcept override { controller_->cancel(owner_); }

    void close() noexcept override {
        if (!closed_.exchange(true, std::memory_order_acq_rel)) {
            controller_->cancel(owner_);
        }
    }

private:
    std::shared_ptr<PrologixController> controller_;
    ResourceDescriptor resource_;
    std::uint64_t owner_;
    std::atomic<bool> closed_{false};
    std::deque<std::uint8_t> response_;
    bool response_end_{false};
};

class PrologixProvider final : public GpibProvider {
public:
    PrologixProvider(ViUInt16 board,
                     std::shared_ptr<PrologixController> controller)
        : board_(board), controller_(std::move(controller)) {}

    std::vector<std::string> discover() override { return {}; }

    std::unique_ptr<GpibTransport> open(const ResourceDescriptor& resource,
                                        ViUInt32 timeout,
                                        ViStatus& status) override {
        if (resource.kind != ResourceKind::gpib_instr ||
            resource.interface_number != board_) {
            status = VI_ERROR_RSRC_NFOUND;
            return nullptr;
        }
        const auto owner = controller_->allocate_owner();
        Operation operation(timeout);
        status = controller_->open(owner, operation);
        if (status < VI_SUCCESS) {
            return nullptr;
        }
        return std::make_unique<PrologixTransport>(controller_, resource, owner);
    }

private:
    ViUInt16 board_;
    std::shared_ptr<PrologixController> controller_;
};

struct ControllerKey {
    PrologixConnectionKind connection;
    std::string endpoint;
    std::uint16_t port;

    auto operator<=>(const ControllerKey&) const = default;
};

struct ControllerPool {
    std::mutex mutex;
    std::map<ControllerKey, std::weak_ptr<PrologixController>> controllers;
};

ControllerPool& controller_pool() {
    static auto* pool = new ControllerPool();
    return *pool;
}

}  // namespace

std::shared_ptr<GpibProvider> make_prologix_provider(
    ViUInt16 board, PrologixConfiguration configuration, ViStatus& status) {
    auto& pool = controller_pool();
    std::lock_guard lock(pool.mutex);
    const ControllerKey key{configuration.connection, configuration.endpoint,
                            configuration.tcp_port};
    std::shared_ptr<PrologixController> controller;
    if (const auto found = pool.controllers.find(key);
        found != pool.controllers.end()) {
        controller = found->second.lock();
        if (!controller) {
            pool.controllers.erase(found);
        } else if (!same_configuration(controller->configuration(), configuration)) {
            status = VI_ERROR_RSRC_BUSY;
            return nullptr;
        }
    }
    if (!controller) {
        controller =
            std::make_shared<PrologixController>(std::move(configuration));
        pool.controllers.emplace(key, controller);
    }
    status = VI_SUCCESS;
    return std::make_shared<PrologixProvider>(board, std::move(controller));
}

}  // namespace wrvisa
