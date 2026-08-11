#ifndef WRVISA_CORE_HANDLE_TABLE_H
#define WRVISA_CORE_HANDLE_TABLE_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "visa.h"

namespace wrvisa {

enum class ObjectType : std::uint8_t {
    resource_manager = 1,
    find_list = 2,
    session = 3,
};

class Object {
public:
    explicit Object(ObjectType type) noexcept : type_(type) {}
    virtual ~Object() = default;

    ObjectType type() const noexcept { return type_; }
    virtual void close() noexcept = 0;

private:
    ObjectType type_;
};

class HandleTable {
public:
    ViObject insert(ObjectType type, std::shared_ptr<Object> object);
    std::shared_ptr<Object> get(ViObject handle, ObjectType expected) const;
    std::shared_ptr<Object> get_any(ViObject handle) const;
    std::shared_ptr<Object> remove(ViObject handle);

private:
    struct Slot {
        std::shared_ptr<Object> object;
        std::uint16_t generation{1};
        ObjectType type{ObjectType::resource_manager};
        bool retired{false};
    };

    static ViObject encode(ObjectType type, std::uint16_t generation,
                           std::uint16_t index_plus_one) noexcept;
    static bool decode(ViObject handle, ObjectType& type, std::uint16_t& generation,
                       std::uint16_t& index_plus_one) noexcept;
    std::shared_ptr<Object> get_locked(ViObject handle, ObjectType* expected) const;

    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
};

HandleTable& handles();

template <typename T>
std::shared_ptr<T> get_handle(ViObject handle, ObjectType expected) {
    return std::static_pointer_cast<T>(handles().get(handle, expected));
}

}  // namespace wrvisa

#endif
