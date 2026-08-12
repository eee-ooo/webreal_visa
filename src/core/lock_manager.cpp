#include "core/lock_manager.h"

#include <chrono>
#include <iomanip>
#include <sstream>

#include "runtime/operation.h"

namespace wrvisa {

bool LockManager::can_grant(const LockState& state, ViSession owner,
                            ViAccessMode lock_type, ViConstKeyId requested_key) {
    if (state.type == VI_NO_LOCK) {
        return true;
    }
    if (lock_type == VI_EXCLUSIVE_LOCK) {
        return state.type == VI_EXCLUSIVE_LOCK && state.exclusive_owner == owner;
    }
    if (state.type != VI_SHARED_LOCK) {
        return false;
    }
    if (state.shared_owners.contains(owner)) {
        return true;
    }
    return requested_key != nullptr && state.shared_key == requested_key;
}

std::string LockManager::next_key() {
    std::ostringstream value;
    value << "WRVISA-" << std::uppercase << std::hex << next_key_id_++;
    return value.str();
}

ViStatus LockManager::acquire(ViSession owner, const std::string& resource,
                              ViAccessMode lock_type, ViUInt32 timeout,
                              ViConstKeyId requested_key, std::string& access_key,
                              Operation* operation) {
    if (lock_type != VI_EXCLUSIVE_LOCK && lock_type != VI_SHARED_LOCK) {
        return VI_ERROR_INV_LOCK_TYPE;
    }
    if (lock_type == VI_EXCLUSIVE_LOCK && requested_key != nullptr &&
        requested_key[0] != '\0') {
        return VI_ERROR_INV_ACCESS_KEY;
    }

    const auto deadline = timeout == VI_TMO_INFINITE
                              ? std::chrono::steady_clock::time_point::max()
                              : std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(timeout);
    std::unique_lock lock(mutex_);
    auto grantable = [&] {
        if (operation != nullptr && operation->completed()) {
            return true;
        }
        const auto found = locks_.find(resource);
        const LockState empty;
        return can_grant(found == locks_.end() ? empty : found->second, owner,
                         lock_type, requested_key);
    };
    while (!grantable()) {
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            condition_.wait(lock, grantable);
        } else if (!condition_.wait_until(lock, deadline, grantable)) {
            return VI_ERROR_TMO;
        }
    }
    if (operation != nullptr && operation->completed()) {
        return operation->result();
    }

    auto& state = locks_[resource];
    if (lock_type == VI_EXCLUSIVE_LOCK) {
        if (state.type == VI_EXCLUSIVE_LOCK) {
            ++state.exclusive_depth;
            return VI_SUCCESS_NESTED_EXCLUSIVE;
        }
        state.type = VI_EXCLUSIVE_LOCK;
        state.exclusive_owner = owner;
        state.exclusive_depth = 1;
        return VI_SUCCESS;
    }

    if (state.type == VI_SHARED_LOCK) {
        auto& depth = state.shared_owners[owner];
        const bool nested = depth != 0;
        ++depth;
        access_key = state.shared_key;
        return nested ? VI_SUCCESS_NESTED_SHARED : VI_SUCCESS;
    }
    state.type = VI_SHARED_LOCK;
    state.shared_key = next_key();
    state.shared_owners[owner] = 1;
    access_key = state.shared_key;
    return VI_SUCCESS;
}

ViStatus LockManager::release(ViSession owner, const std::string& resource) {
    std::lock_guard lock(mutex_);
    const auto found = locks_.find(resource);
    if (found == locks_.end()) {
        return VI_ERROR_SESN_NLOCKED;
    }
    auto& state = found->second;
    if (state.type == VI_EXCLUSIVE_LOCK && state.exclusive_owner == owner) {
        if (--state.exclusive_depth == 0) {
            locks_.erase(found);
            condition_.notify_all();
        }
        return VI_SUCCESS;
    }
    if (state.type == VI_SHARED_LOCK) {
        const auto shared = state.shared_owners.find(owner);
        if (shared != state.shared_owners.end()) {
            if (--shared->second == 0) {
                state.shared_owners.erase(shared);
            }
            if (state.shared_owners.empty()) {
                locks_.erase(found);
                condition_.notify_all();
            }
            return VI_SUCCESS;
        }
    }
    return VI_ERROR_SESN_NLOCKED;
}

void LockManager::release_all(ViSession owner, const std::string& resource) noexcept {
    std::lock_guard lock(mutex_);
    const auto found = locks_.find(resource);
    if (found == locks_.end()) {
        return;
    }
    auto& state = found->second;
    bool changed = false;
    if (state.type == VI_EXCLUSIVE_LOCK && state.exclusive_owner == owner) {
        locks_.erase(found);
        changed = true;
    } else if (state.type == VI_SHARED_LOCK) {
        changed = state.shared_owners.erase(owner) != 0;
        if (state.shared_owners.empty()) {
            locks_.erase(found);
        }
    }
    if (changed) {
        condition_.notify_all();
    }
}

bool LockManager::can_access(ViSession owner, const std::string& resource) const {
    std::lock_guard lock(mutex_);
    const auto found = locks_.find(resource);
    if (found == locks_.end()) {
        return true;
    }
    const auto& state = found->second;
    if (state.type == VI_EXCLUSIVE_LOCK) {
        return state.exclusive_owner == owner;
    }
    return state.shared_owners.contains(owner);
}

ViAccessMode LockManager::owned_lock_type(ViSession owner,
                                           const std::string& resource) const {
    std::lock_guard lock(mutex_);
    const auto found = locks_.find(resource);
    if (found == locks_.end()) {
        return VI_NO_LOCK;
    }
    if (found->second.type == VI_EXCLUSIVE_LOCK &&
        found->second.exclusive_owner == owner) {
        return VI_EXCLUSIVE_LOCK;
    }
    if (found->second.type == VI_SHARED_LOCK &&
        found->second.shared_owners.contains(owner)) {
        return VI_SHARED_LOCK;
    }
    return VI_NO_LOCK;
}

std::uint32_t LockManager::owned_lock_depth(ViSession owner,
                                            const std::string& resource) const {
    std::lock_guard lock(mutex_);
    const auto found = locks_.find(resource);
    if (found == locks_.end()) {
        return 0;
    }
    if (found->second.type == VI_EXCLUSIVE_LOCK &&
        found->second.exclusive_owner == owner) {
        return found->second.exclusive_depth;
    }
    if (found->second.type == VI_SHARED_LOCK) {
        const auto shared = found->second.shared_owners.find(owner);
        if (shared != found->second.shared_owners.end()) {
            return shared->second;
        }
    }
    return 0;
}

void LockManager::notify_waiters() noexcept {
    // Operation cancellation is stored outside LockManager.  Taking this
    // mutex pairs the state change notification with acquire()'s predicate
    // check and prevents a cancellation wake-up from being lost.
    std::lock_guard lock(mutex_);
    condition_.notify_all();
}

LockManager& lock_manager() {
    static LockManager instance;
    return instance;
}

}  // namespace wrvisa
