#include "core/objects.h"

#include <cstring>
#include <utility>

#include "core/lock_manager.h"
#include "platform/serial_discovery.h"
#include "webreal_visa_ext.h"

namespace wrvisa {
namespace {

void copy_string(void* destination, const std::string& value) {
    auto* output = static_cast<ViChar*>(destination);
    const auto amount = value.size() < VI_FIND_BUFLEN - 1u
                            ? value.size()
                            : static_cast<std::size_t>(VI_FIND_BUFLEN - 1u);
    std::memcpy(output, value.data(), amount);
    output[amount] = '\0';
}

class OperationFinish {
public:
    OperationFinish(SessionObject& session, std::shared_ptr<Operation> operation)
        : session_(session), operation_(std::move(operation)) {}
    ~OperationFinish() { session_.finish_operation(operation_); }

private:
    SessionObject& session_;
    std::shared_ptr<Operation> operation_;
};

}  // namespace

ResourceManager::ResourceManager()
    : Object(ObjectType::resource_manager), serial_paths_(discover_serial_ports()) {}

bool ResourceManager::add_child(ViObject child) {
    std::lock_guard lock(mutex_);
    if (closed_) {
        return false;
    }
    children_.insert(child);
    return true;
}

void ResourceManager::remove_child(ViObject child) noexcept {
    std::lock_guard lock(mutex_);
    children_.erase(child);
}

std::vector<ViObject> ResourceManager::take_children() noexcept {
    std::lock_guard lock(mutex_);
    closed_ = true;
    std::vector<ViObject> result(children_.begin(), children_.end());
    children_.clear();
    return result;
}

bool ResourceManager::set_serial_path(ViUInt16 interface_number, std::string path) {
    std::lock_guard lock(mutex_);
    if (closed_ || path.empty()) {
        return false;
    }
    serial_paths_[interface_number] = std::move(path);
    return true;
}

std::optional<std::string> ResourceManager::serial_path(
    ViUInt16 interface_number) const {
    std::lock_guard lock(mutex_);
    const auto found = serial_paths_.find(interface_number);
    if (closed_ || found == serial_paths_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<std::string> ResourceManager::discoverable_resources() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> resources;
    if (closed_) {
        return resources;
    }
    resources.reserve(serial_paths_.size());
    for (const auto& [number, path] : serial_paths_) {
        static_cast<void>(path);
        resources.push_back("ASRL" + std::to_string(number) + "::INSTR");
    }
    return resources;
}

void ResourceManager::close() noexcept {
    for (const auto child : take_children()) {
        auto object = handles().remove(child);
        if (object) {
            object->close();
        }
    }
}

bool FindListObject::next(std::string& result) {
    std::lock_guard lock(mutex_);
    if (closed_ || index_ >= matches_.size()) {
        return false;
    }
    result = matches_[index_++];
    return true;
}

void FindListObject::close() noexcept {
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        matches_.clear();
    }
    if (auto parent = parent_.lock()) {
        parent->remove_child(handle_);
    }
}

SessionObject::SessionObject(std::weak_ptr<ResourceManager> parent,
                             ViSession parent_handle, ResourceDescriptor descriptor,
                             std::unique_ptr<BackendSession> backend)
    : Object(ObjectType::session),
      parent_(std::move(parent)),
      parent_handle_(parent_handle),
      descriptor_(std::move(descriptor)),
      backend_(std::move(backend)) {}

std::shared_ptr<Operation> SessionObject::begin_operation() {
    ViUInt32 timeout = 0;
    {
        std::lock_guard lock(attributes_mutex_);
        timeout = timeout_;
    }
    return begin_operation(timeout);
}

std::shared_ptr<Operation> SessionObject::begin_operation(ViUInt32 timeout) {
    auto operation = std::make_shared<Operation>(timeout);
    std::lock_guard lock(operations_mutex_);
    if (closed_) {
        static_cast<void>(operation->request_cancel());
        return operation;
    }
    operations_.emplace(next_operation_id_++, operation);
    return operation;
}

std::size_t SessionObject::active_operation_count() const noexcept {
    std::lock_guard lock(operations_mutex_);
    return operations_.size();
}

void SessionObject::finish_operation(
    const std::shared_ptr<Operation>& operation) noexcept {
    std::lock_guard lock(operations_mutex_);
    for (auto found = operations_.begin(); found != operations_.end(); ++found) {
        if (found->second == operation) {
            operations_.erase(found);
            break;
        }
    }
    operations_condition_.notify_all();
}

ViStatus SessionObject::cancel_operations() noexcept {
    std::vector<std::shared_ptr<Operation>> active;
    {
        std::lock_guard lock(operations_mutex_);
        active.reserve(operations_.size());
        for (const auto& [id, operation] : operations_) {
            static_cast<void>(id);
            active.push_back(operation);
        }
    }
    for (const auto& operation : active) {
        static_cast<void>(operation->request_cancel());
    }
    backend_->notify_cancel();
    lock_manager().notify_waiters();
    return VI_SUCCESS;
}

bool SessionObject::can_access() const {
    return lock_manager().can_access(handle_, descriptor_.canonical_name);
}

ViStatus SessionObject::read(ViPBuf buffer, ViUInt32 count, ViPUInt32 return_count) {
    if (!can_access()) {
        return VI_ERROR_RSRC_LOCKED;
    }
    auto operation = begin_operation();
    OperationFinish finish(*this, operation);
    if (operation->completed()) {
        if (return_count != nullptr) {
            *return_count = 0;
        }
        return operation->result();
    }
    ReadOptions options;
    {
        std::lock_guard lock(attributes_mutex_);
        options.termchar_enabled = termchar_enabled_ == VI_TRUE;
        options.termchar = termchar_;
    }
    return backend_->read(*operation, buffer, count, return_count, options);
}

ViStatus SessionObject::write(ViConstBuf buffer, ViUInt32 count,
                              ViPUInt32 return_count) {
    if (!can_access()) {
        return VI_ERROR_RSRC_LOCKED;
    }
    auto operation = begin_operation();
    OperationFinish finish(*this, operation);
    if (operation->completed()) {
        if (return_count != nullptr) {
            *return_count = 0;
        }
        return operation->result();
    }
    return backend_->write(*operation, buffer, count, return_count);
}

ViStatus SessionObject::clear() {
    return can_access() ? backend_->clear() : VI_ERROR_RSRC_LOCKED;
}

ViStatus SessionObject::flush(ViUInt16 mask) {
    return can_access() ? backend_->flush(mask) : VI_ERROR_RSRC_LOCKED;
}

ViStatus SessionObject::read_stb(ViPUInt16 status) {
    return can_access() ? backend_->read_stb(status) : VI_ERROR_RSRC_LOCKED;
}

ViStatus SessionObject::assert_trigger(ViUInt16 protocol) {
    return can_access() ? backend_->assert_trigger(protocol) : VI_ERROR_RSRC_LOCKED;
}

ViStatus SessionObject::lock(ViAccessMode lock_type, ViUInt32 timeout,
                             ViConstKeyId requested_key, std::string& access_key) {
    auto operation = begin_operation(timeout);
    OperationFinish finish(*this, operation);
    if (operation->completed()) {
        return operation->result();
    }
    const auto status = lock_manager().acquire(
        handle_, descriptor_.canonical_name, lock_type, timeout, requested_key,
        access_key, operation.get());
    if (status < VI_SUCCESS) {
        static_cast<void>(operation->try_complete(status));
        return operation->result();
    }
    if (!operation->try_complete(status)) {
        static_cast<void>(lock_manager().release(handle_, descriptor_.canonical_name));
        return operation->result();
    }
    return status;
}

ViStatus SessionObject::unlock() {
    return lock_manager().release(handle_, descriptor_.canonical_name);
}

ViStatus SessionObject::set_attribute(ViAttr attribute, ViAttrState value) {
    std::lock_guard lock(attributes_mutex_);
    switch (attribute) {
        case VI_ATTR_TMO_VALUE:
            if (value > UINT32_MAX) {
                return VI_ERROR_NSUP_ATTR_STATE;
            }
            timeout_ = static_cast<ViUInt32>(value);
            return VI_SUCCESS;
        case VI_ATTR_TERMCHAR:
            if (value > UINT8_MAX) {
                return VI_ERROR_NSUP_ATTR_STATE;
            }
            termchar_ = static_cast<ViByte>(value);
            return VI_SUCCESS;
        case VI_ATTR_TERMCHAR_EN:
            if (value != VI_FALSE && value != VI_TRUE) {
                return VI_ERROR_NSUP_ATTR_STATE;
            }
            termchar_enabled_ = static_cast<ViBoolean>(value);
            return VI_SUCCESS;
        case VI_ATTR_SEND_END_EN:
            if (value != VI_FALSE && value != VI_TRUE) {
                return VI_ERROR_NSUP_ATTR_STATE;
            }
            send_end_enabled_ = static_cast<ViBoolean>(value);
            return VI_SUCCESS;
        case VI_ATTR_USER_DATA:
            user_data_ = value;
            return VI_SUCCESS;
        case VI_ATTR_RSRC_CLASS:
        case VI_ATTR_RSRC_NAME:
        case VI_ATTR_RSRC_IMPL_VERSION:
        case VI_ATTR_RSRC_LOCK_STATE:
        case VI_ATTR_RD_BUF_SIZE:
        case VI_ATTR_WR_BUF_SIZE:
        case VI_ATTR_RM_SESSION:
        case VI_ATTR_RSRC_SPEC_VERSION:
        case VI_ATTR_INTF_TYPE:
        case VI_ATTR_INTF_NUM:
            return VI_ERROR_ATTR_READONLY;
        default:
            return backend_->set_attribute(attribute, value);
    }
}

ViStatus SessionObject::get_attribute(ViAttr attribute, void* value) const {
    if (value == nullptr) {
        return VI_ERROR_INV_PARAMETER;
    }
    std::lock_guard lock(attributes_mutex_);
    switch (attribute) {
        case VI_ATTR_RSRC_CLASS:
            copy_string(value, descriptor_.resource_class);
            return VI_SUCCESS;
        case VI_ATTR_RSRC_NAME:
            copy_string(value, descriptor_.canonical_name);
            return VI_SUCCESS;
        case VI_ATTR_RSRC_IMPL_VERSION:
            *static_cast<ViVersion*>(value) = UINT32_C(0x00000200);
            return VI_SUCCESS;
        case VI_ATTR_RSRC_LOCK_STATE:
            *static_cast<ViAccessMode*>(value) =
                lock_manager().owned_lock_type(handle_, descriptor_.canonical_name);
            return VI_SUCCESS;
        case VI_ATTR_USER_DATA:
            *static_cast<ViAttrState*>(value) = user_data_;
            return VI_SUCCESS;
        case VI_ATTR_SEND_END_EN:
            *static_cast<ViBoolean*>(value) = send_end_enabled_;
            return VI_SUCCESS;
        case VI_ATTR_TERMCHAR:
            *static_cast<ViUInt8*>(value) = termchar_;
            return VI_SUCCESS;
        case VI_ATTR_TMO_VALUE:
            *static_cast<ViUInt32*>(value) = timeout_;
            return VI_SUCCESS;
        case VI_ATTR_RD_BUF_SIZE:
            *static_cast<ViUInt32*>(value) = read_buffer_size_;
            return VI_SUCCESS;
        case VI_ATTR_WR_BUF_SIZE:
            *static_cast<ViUInt32*>(value) = write_buffer_size_;
            return VI_SUCCESS;
        case VI_ATTR_TERMCHAR_EN:
            *static_cast<ViBoolean*>(value) = termchar_enabled_;
            return VI_SUCCESS;
        case VI_ATTR_RM_SESSION:
            *static_cast<ViSession*>(value) = parent_handle_;
            return VI_SUCCESS;
        case VI_ATTR_RSRC_SPEC_VERSION:
            *static_cast<ViVersion*>(value) = VI_SPEC_VERSION;
            return VI_SUCCESS;
        case VI_ATTR_INTF_TYPE:
            *static_cast<ViUInt16*>(value) = descriptor_.interface_type;
            return VI_SUCCESS;
        case VI_ATTR_INTF_NUM:
            *static_cast<ViUInt16*>(value) = descriptor_.interface_number;
            return VI_SUCCESS;
        default:
            return backend_->get_attribute(attribute, value);
    }
}

ViStatus SessionObject::set_buffer(ViUInt16 mask, ViUInt32 size) {
    if (size == 0) {
        return VI_ERROR_INV_SIZE;
    }
    constexpr ViUInt16 kAllowed = VI_READ_BUF | VI_WRITE_BUF | VI_IO_IN_BUF |
                                  VI_IO_OUT_BUF;
    if (mask == 0 || (mask & static_cast<ViUInt16>(~kAllowed)) != 0) {
        return VI_ERROR_INV_MASK;
    }
    std::lock_guard lock(attributes_mutex_);
    if ((mask & (VI_READ_BUF | VI_IO_IN_BUF)) != 0) {
        read_buffer_size_ = size;
    }
    if ((mask & (VI_WRITE_BUF | VI_IO_OUT_BUF)) != 0) {
        write_buffer_size_ = size;
    }
    return VI_SUCCESS;
}

void SessionObject::close() noexcept {
    {
        std::lock_guard lock(operations_mutex_);
        closed_ = true;
        for (const auto& [id, operation] : operations_) {
            static_cast<void>(id);
            static_cast<void>(operation->request_cancel());
        }
    }
    backend_->notify_cancel();
    lock_manager().notify_waiters();
    {
        std::unique_lock lock(operations_mutex_);
        operations_condition_.wait(lock, [this] { return operations_.empty(); });
    }
    backend_->close();
    lock_manager().release_all(handle_, descriptor_.canonical_name);
    if (auto parent = parent_.lock()) {
        parent->remove_child(handle_);
    }
}

void close_handle_object(ViObject handle, const std::shared_ptr<Object>& object) noexcept {
    static_cast<void>(handle);
    object->close();
}

}  // namespace wrvisa
