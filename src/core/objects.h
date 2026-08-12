#ifndef WRVISA_CORE_OBJECTS_H
#define WRVISA_CORE_OBJECTS_H

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/backend_session.h"
#include "core/handle_table.h"
#include "resource/resource_parser.h"

namespace wrvisa {

struct ResolvedResource {
    ResourceDescriptor descriptor;
    std::string alias;
};

class ResourceManager final : public Object {
public:
    ResourceManager();

    bool add_child(ViObject child);
    void remove_child(ViObject child) noexcept;
    std::vector<ViObject> take_children() noexcept;
    bool set_serial_path(ViUInt16 interface_number, std::string path);
    std::optional<std::string> serial_path(ViUInt16 interface_number) const;
    bool set_tcpip_service_port(std::string host, TcpipProtocol protocol,
                                ViUInt16 port);
    std::optional<ViUInt16> tcpip_service_port(
        const std::string& host, TcpipProtocol protocol) const;
    bool set_resource_alias(std::string alias, ResourceDescriptor resource);
    std::optional<ResolvedResource> resolve_resource(std::string_view name) const;
    std::vector<std::string> discoverable_resources() const;
    void close() noexcept override;

private:
    mutable std::mutex mutex_;
    std::unordered_set<ViObject> children_;
    std::map<ViUInt16, std::string> serial_paths_;
    std::map<std::pair<std::string, TcpipProtocol>, ViUInt16>
        tcpip_service_ports_;
    struct AliasEntry {
        std::string display_name;
        ResourceDescriptor resource;
    };
    std::map<std::string, AliasEntry> aliases_;
    std::map<std::string, std::string> aliases_by_resource_;
    bool closed_{false};
};

class FindListObject final : public Object {
public:
    FindListObject(std::weak_ptr<ResourceManager> parent, ViSession parent_handle,
                   std::vector<std::string> matches)
        : Object(ObjectType::find_list),
          parent_(std::move(parent)),
          parent_handle_(parent_handle),
          matches_(std::move(matches)) {}

    bool next(std::string& result);
    void set_handle(ViFindList handle) noexcept { handle_ = handle; }
    void close() noexcept override;

private:
    std::weak_ptr<ResourceManager> parent_;
    ViSession parent_handle_{VI_NULL};
    ViFindList handle_{VI_NULL};
    std::mutex mutex_;
    std::vector<std::string> matches_;
    std::size_t index_{1};
    bool closed_{false};
};

class SessionObject final : public Object {
public:
    SessionObject(std::weak_ptr<ResourceManager> parent, ViSession parent_handle,
                  ResourceDescriptor descriptor,
                  std::unique_ptr<BackendSession> backend);

    void set_handle(ViSession handle) noexcept { handle_ = handle; }
    ViSession handle() const noexcept { return handle_; }
    ViSession parent_handle() const noexcept { return parent_handle_; }
    const ResourceDescriptor& descriptor() const noexcept { return descriptor_; }

    std::shared_ptr<Operation> begin_operation();
    std::shared_ptr<Operation> begin_operation(ViUInt32 timeout);
    void finish_operation(const std::shared_ptr<Operation>& operation) noexcept;
    std::size_t active_operation_count() const noexcept;
    ViStatus cancel_operations() noexcept;
    bool can_access() const;

    ViStatus read(ViPBuf buffer, ViUInt32 count, ViPUInt32 return_count);
    ViStatus write(ViConstBuf buffer, ViUInt32 count, ViPUInt32 return_count);
    ViStatus clear();
    ViStatus flush(ViUInt16 mask);
    ViStatus read_stb(ViPUInt16 status);
    ViStatus assert_trigger(ViUInt16 protocol);
    ViStatus lock(ViAccessMode lock_type, ViUInt32 timeout,
                  ViConstKeyId requested_key, std::string& access_key);
    ViStatus unlock();
    ViStatus set_attribute(ViAttr attribute, ViAttrState value);
    ViStatus get_attribute(ViAttr attribute, void* value) const;
    ViStatus set_buffer(ViUInt16 mask, ViUInt32 size);
    void close() noexcept override;

private:
    std::weak_ptr<ResourceManager> parent_;
    ViSession parent_handle_{VI_NULL};
    ViSession handle_{VI_NULL};
    ResourceDescriptor descriptor_;
    std::unique_ptr<BackendSession> backend_;

    mutable std::mutex attributes_mutex_;
    ViUInt32 timeout_{2000};
    ViBoolean termchar_enabled_{VI_FALSE};
    ViByte termchar_{'\n'};
    ViBoolean send_end_enabled_{VI_TRUE};
    ViAttrState user_data_{0};
    ViUInt32 read_buffer_size_{4096};
    ViUInt32 write_buffer_size_{4096};

    mutable std::mutex operations_mutex_;
    std::condition_variable operations_condition_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Operation>> operations_;
    std::uint64_t next_operation_id_{1};
    bool closed_{false};
};

void close_handle_object(ViObject handle, const std::shared_ptr<Object>& object) noexcept;

}  // namespace wrvisa

#endif
