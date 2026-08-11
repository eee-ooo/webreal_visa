#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "core/handle_table.h"
#include "core/lock_manager.h"
#include "runtime/operation.h"
#include "test_support.h"

namespace {

class TestObject final : public wrvisa::Object {
public:
    explicit TestObject(wrvisa::ObjectType type) : Object(type) {}
    void close() noexcept override { closed = true; }
    bool closed{false};
};

}  // namespace

int main() {
    wrvisa::HandleTable table;
    auto first = std::make_shared<TestObject>(wrvisa::ObjectType::session);
    const auto stale = table.insert(wrvisa::ObjectType::session, first);
    CHECK(table.get(stale, wrvisa::ObjectType::session) == first);
    CHECK(!table.get(stale, wrvisa::ObjectType::find_list));
    CHECK(table.remove(stale) == first);
    CHECK(!table.get_any(stale));

    auto second = std::make_shared<TestObject>(wrvisa::ObjectType::session);
    const auto current = table.insert(wrvisa::ObjectType::session, second);
    CHECK(current != stale);
    CHECK(!table.get_any(stale));
    CHECK(table.get(current, wrvisa::ObjectType::session) == second);

    wrvisa::Operation operation(1000);
    std::atomic<int> winners{0};
    std::thread normal([&] {
        if (operation.try_complete(VI_SUCCESS)) {
            ++winners;
        }
    });
    std::thread cancel([&] {
        if (operation.request_cancel()) {
            ++winners;
        }
    });
    normal.join();
    cancel.join();
    CHECK(winners == 1);
    CHECK(operation.completed());
    CHECK(operation.result() == VI_SUCCESS || operation.result() == VI_ERROR_ABORT);

    wrvisa::Operation callback_operation(1000);
    std::atomic<int> callbacks{0};
    callback_operation.set_cancel_handler([&] { ++callbacks; });
    CHECK(callback_operation.request_cancel());
    CHECK(callbacks == 1);
    CHECK(!callback_operation.request_cancel());
    CHECK(callbacks == 1);
    callback_operation.clear_cancel_handler();

    wrvisa::Operation timeout_operation(1000);
    std::atomic<int> timeout_callbacks{0};
    timeout_operation.set_cancel_handler([&] { ++timeout_callbacks; });
    CHECK(timeout_operation.request_timeout());
    CHECK(timeout_operation.result() == VI_ERROR_TMO);
    CHECK(timeout_callbacks == 1);
    CHECK(!timeout_operation.request_cancel());
    CHECK(timeout_callbacks == 1);

    wrvisa::LockManager locks;
    std::string key;
    CHECK(locks.acquire(11, "resource", VI_SHARED_LOCK, 0, nullptr, key) ==
          VI_SUCCESS);
    CHECK(!key.empty());
    std::string joined_key;
    CHECK(locks.acquire(12, "resource", VI_SHARED_LOCK, 0, key.c_str(), joined_key) ==
          VI_SUCCESS);
    CHECK(joined_key == key);
    CHECK(locks.can_access(11, "resource"));
    CHECK(locks.can_access(12, "resource"));
    CHECK(!locks.can_access(13, "resource"));
    CHECK(locks.release(11, "resource") == VI_SUCCESS);
    CHECK(locks.release(12, "resource") == VI_SUCCESS);
    CHECK(locks.acquire(13, "resource", VI_EXCLUSIVE_LOCK, 0, nullptr, key) ==
          VI_SUCCESS);
    CHECK(locks.acquire(13, "resource", VI_EXCLUSIVE_LOCK, 0, nullptr, key) ==
          VI_SUCCESS_NESTED_EXCLUSIVE);
    CHECK(locks.release(13, "resource") == VI_SUCCESS);
    CHECK(locks.release(13, "resource") == VI_SUCCESS);
    CHECK(locks.release(13, "resource") == VI_ERROR_SESN_NLOCKED);
    return 0;
}
