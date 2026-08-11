#ifndef WRVISA_CORE_LOCK_MANAGER_H
#define WRVISA_CORE_LOCK_MANAGER_H

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "visa.h"

namespace wrvisa { class Operation; }

namespace wrvisa {

class LockManager {
public:
    ViStatus acquire(ViSession owner, const std::string& resource,
                     ViAccessMode lock_type, ViUInt32 timeout,
                     ViConstKeyId requested_key, std::string& access_key,
                     Operation* operation = nullptr);
    ViStatus release(ViSession owner, const std::string& resource);
    void release_all(ViSession owner, const std::string& resource) noexcept;
    bool can_access(ViSession owner, const std::string& resource) const;
    ViAccessMode owned_lock_type(ViSession owner, const std::string& resource) const;
    void notify_waiters() noexcept;

private:
    struct LockState {
        ViAccessMode type{VI_NO_LOCK};
        ViSession exclusive_owner{VI_NULL};
        std::uint32_t exclusive_depth{0};
        std::string shared_key;
        std::unordered_map<ViSession, std::uint32_t> shared_owners;
    };

    static bool can_grant(const LockState& state, ViSession owner,
                          ViAccessMode lock_type, ViConstKeyId requested_key);
    std::string next_key();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_map<std::string, LockState> locks_;
    std::uint64_t next_key_id_{1};
};

LockManager& lock_manager();

}  // namespace wrvisa

#endif
