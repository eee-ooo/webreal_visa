#include "core/handle_table.h"

#include <limits>
#include <stdexcept>

namespace wrvisa {
namespace {

constexpr std::uint32_t kTypeShift = 28;
constexpr std::uint32_t kGenerationShift = 16;
constexpr std::uint32_t kTypeMask = 0x0Fu;
constexpr std::uint32_t kGenerationMask = 0x0FFFu;
constexpr std::size_t kMaxSlots = 0xFFFFu;

}  // namespace

ViObject HandleTable::encode(ObjectType type, std::uint16_t generation,
                             std::uint16_t index_plus_one) noexcept {
    return (static_cast<ViObject>(type) << kTypeShift) |
           (static_cast<ViObject>(generation) << kGenerationShift) |
           static_cast<ViObject>(index_plus_one);
}

bool HandleTable::decode(ViObject handle, ObjectType& type, std::uint16_t& generation,
                         std::uint16_t& index_plus_one) noexcept {
    const auto raw_type = static_cast<std::uint8_t>((handle >> kTypeShift) & kTypeMask);
    const auto raw_generation =
        static_cast<std::uint16_t>((handle >> kGenerationShift) & kGenerationMask);
    const auto raw_index = static_cast<std::uint16_t>(handle & 0xFFFFu);
    if (raw_index == 0 || raw_generation == 0 || raw_type < 1 || raw_type > 3) {
        return false;
    }
    type = static_cast<ObjectType>(raw_type);
    generation = raw_generation;
    index_plus_one = raw_index;
    return true;
}

ViObject HandleTable::insert(ObjectType type, std::shared_ptr<Object> object) {
    if (!object || object->type() != type) {
        throw std::invalid_argument("invalid object for handle table");
    }

    std::lock_guard lock(mutex_);
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        auto& slot = slots_[index];
        if (!slot.object && !slot.retired) {
            slot.type = type;
            slot.object = std::move(object);
            return encode(type, slot.generation,
                          static_cast<std::uint16_t>(index + 1u));
        }
    }
    if (slots_.size() >= kMaxSlots) {
        throw std::bad_alloc();
    }
    Slot slot;
    slot.type = type;
    slot.object = std::move(object);
    slots_.push_back(std::move(slot));
    return encode(type, 1, static_cast<std::uint16_t>(slots_.size()));
}

std::shared_ptr<Object> HandleTable::get_locked(ViObject handle,
                                                 ObjectType* expected) const {
    ObjectType type{};
    std::uint16_t generation = 0;
    std::uint16_t index_plus_one = 0;
    if (!decode(handle, type, generation, index_plus_one)) {
        return {};
    }
    if (expected != nullptr && type != *expected) {
        return {};
    }
    const auto index = static_cast<std::size_t>(index_plus_one - 1u);
    if (index >= slots_.size()) {
        return {};
    }
    const auto& slot = slots_[index];
    if (!slot.object || slot.retired || slot.type != type ||
        slot.generation != generation) {
        return {};
    }
    return slot.object;
}

std::shared_ptr<Object> HandleTable::get(ViObject handle, ObjectType expected) const {
    std::lock_guard lock(mutex_);
    return get_locked(handle, &expected);
}

std::shared_ptr<Object> HandleTable::get_any(ViObject handle) const {
    std::lock_guard lock(mutex_);
    return get_locked(handle, nullptr);
}

std::shared_ptr<Object> HandleTable::remove(ViObject handle) {
    std::lock_guard lock(mutex_);
    auto object = get_locked(handle, nullptr);
    if (!object) {
        return {};
    }
    ObjectType type{};
    std::uint16_t generation = 0;
    std::uint16_t index_plus_one = 0;
    static_cast<void>(decode(handle, type, generation, index_plus_one));
    auto& slot = slots_[static_cast<std::size_t>(index_plus_one - 1u)];
    slot.object.reset();
    if (slot.generation == kGenerationMask) {
        slot.retired = true;
    } else {
        ++slot.generation;
    }
    return object;
}

HandleTable& handles() {
    static HandleTable table;
    return table;
}

}  // namespace wrvisa
