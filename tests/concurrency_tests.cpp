#include <array>
#include <chrono>
#include <future>
#include <thread>

#include "core/handle_table.h"
#include "core/objects.h"
#include "test_support.h"
#include "visa.h"
#include "webreal_visa_ext.h"

namespace {

ViSession open_mock(ViSession rm) {
    ViSession session = VI_NULL;
    CHECK(viOpen(rm, WRVISA_MOCK_RESOURCE, VI_NO_LOCK, 1000, &session) == VI_SUCCESS);
    return session;
}

std::shared_ptr<wrvisa::SessionObject> session_object(ViSession session) {
    auto object = wrvisa::get_handle<wrvisa::SessionObject>(
        session, wrvisa::ObjectType::session);
    CHECK(object != nullptr);
    return object;
}

void wait_until_active(const std::shared_ptr<wrvisa::SessionObject>& session) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (session->active_operation_count() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(session->active_operation_count() != 0);
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    ViSession rm = VI_NULL;
    CHECK(viOpenDefaultRM(&rm) == VI_SUCCESS);

    ViSession canceled = open_mock(rm);
    auto canceled_object = session_object(canceled);
    CHECK(viSetAttribute(canceled, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    std::array<ViByte, 8> canceled_buffer{};
    canceled_buffer.fill(0xA5);
    ViUInt32 canceled_count = 77;
    auto canceled_read = std::async(std::launch::async, [&] {
        return viRead(canceled, canceled_buffer.data(),
                      static_cast<ViUInt32>(canceled_buffer.size()), &canceled_count);
    });
    wait_until_active(canceled_object);
    CHECK(viTerminate(canceled, VI_NULL, VI_NULL) == VI_SUCCESS);
    CHECK(canceled_read.wait_for(1s) == std::future_status::ready);
    CHECK(canceled_read.get() == VI_ERROR_ABORT);
    CHECK(canceled_count == 0);
    for (const auto byte : canceled_buffer) {
        CHECK(byte == 0xA5);
    }
    CHECK(viClose(canceled) == VI_SUCCESS);

    ViSession timed = open_mock(rm);
    CHECK(viSetAttribute(timed, VI_ATTR_TMO_VALUE, 30) == VI_SUCCESS);
    std::array<ViByte, 4> timed_buffer{};
    ViUInt32 timed_count = 99;
    const auto started = std::chrono::steady_clock::now();
    CHECK(viRead(timed, timed_buffer.data(), static_cast<ViUInt32>(timed_buffer.size()),
                 &timed_count) == VI_ERROR_TMO);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed >= 20ms);
    CHECK(elapsed < 1s);
    CHECK(timed_count == 0);
    CHECK(viClose(timed) == VI_SUCCESS);

    ViSession closing = open_mock(rm);
    auto closing_object = session_object(closing);
    CHECK(viSetAttribute(closing, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    std::array<ViByte, 4> close_buffer{};
    close_buffer.fill(0x3C);
    ViUInt32 close_count = 44;
    auto close_read = std::async(std::launch::async, [&] {
        return viRead(closing, close_buffer.data(),
                      static_cast<ViUInt32>(close_buffer.size()), &close_count);
    });
    wait_until_active(closing_object);
    CHECK(viClose(closing) == VI_SUCCESS);
    CHECK(close_read.wait_for(1s) == std::future_status::ready);
    CHECK(close_read.get() == VI_ERROR_ABORT);
    CHECK(close_count == 0);
    for (const auto byte : close_buffer) {
        CHECK(byte == 0x3C);
    }

    ViSession child = open_mock(rm);
    auto child_object = session_object(child);
    CHECK(viSetAttribute(child, VI_ATTR_TMO_VALUE, 5000) == VI_SUCCESS);
    ViUInt32 child_count = 1;
    std::array<ViByte, 2> child_buffer{};
    auto child_read = std::async(std::launch::async, [&] {
        return viRead(child, child_buffer.data(), static_cast<ViUInt32>(child_buffer.size()),
                      &child_count);
    });
    wait_until_active(child_object);
    CHECK(viClose(rm) == VI_SUCCESS);
    CHECK(child_read.wait_for(1s) == std::future_status::ready);
    CHECK(child_read.get() == VI_ERROR_ABORT);
    CHECK(viClose(child) == VI_ERROR_INV_OBJECT);

    ViSession lock_rm = VI_NULL;
    CHECK(viOpenDefaultRM(&lock_rm) == VI_SUCCESS);
    ViSession lock_owner = open_mock(lock_rm);
    ViSession lock_waiter = open_mock(lock_rm);
    CHECK(viLock(lock_owner, VI_EXCLUSIVE_LOCK, 0, nullptr, nullptr) == VI_SUCCESS);
    auto waiter_object = session_object(lock_waiter);
    auto waiting_lock = std::async(std::launch::async, [&] {
        return viLock(lock_waiter, VI_EXCLUSIVE_LOCK, 5000, nullptr, nullptr);
    });
    wait_until_active(waiter_object);
    CHECK(viClose(lock_waiter) == VI_SUCCESS);
    CHECK(waiting_lock.wait_for(1s) == std::future_status::ready);
    CHECK(waiting_lock.get() == VI_ERROR_ABORT);
    CHECK(viUnlock(lock_owner) == VI_SUCCESS);
    CHECK(viClose(lock_owner) == VI_SUCCESS);
    CHECK(viClose(lock_rm) == VI_SUCCESS);
    return 0;
}
