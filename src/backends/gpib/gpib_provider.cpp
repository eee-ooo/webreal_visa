#include "backends/gpib/gpib_provider.h"

#include <algorithm>
#include <utility>

namespace wrvisa {
namespace {

struct RegistryState {
    std::mutex mutex;
    std::uint64_t next_id{1};
    std::map<std::uint64_t, std::shared_ptr<GpibProvider>> providers;
};

RegistryState& registry() {
    static auto* state = new RegistryState();
    return *state;
}

std::vector<std::shared_ptr<GpibProvider>> provider_snapshot() {
    auto& state = registry();
    std::lock_guard lock(state.mutex);
    std::vector<std::shared_ptr<GpibProvider>> result;
    result.reserve(state.providers.size());
    for (const auto& [id, provider] : state.providers) {
        static_cast<void>(id);
        result.push_back(provider);
    }
    return result;
}

void unregister_provider(std::uint64_t id) noexcept {
    auto& state = registry();
    std::lock_guard lock(state.mutex);
    state.providers.erase(id);
}

}  // namespace

GpibProviderRegistration::GpibProviderRegistration(
    GpibProviderRegistration&& other) noexcept
    : id_(std::exchange(other.id_, 0)) {}

GpibProviderRegistration& GpibProviderRegistration::operator=(
    GpibProviderRegistration&& other) noexcept {
    if (this != &other) {
        reset();
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

GpibProviderRegistration::~GpibProviderRegistration() { reset(); }

void GpibProviderRegistration::reset() noexcept {
    if (id_ != 0) {
        unregister_provider(std::exchange(id_, 0));
    }
}

GpibProviderRegistration register_gpib_provider(
    std::shared_ptr<GpibProvider> provider) {
    if (!provider) {
        return {};
    }
    auto& state = registry();
    std::lock_guard lock(state.mutex);
    const auto id = state.next_id++;
    state.providers.emplace(id, std::move(provider));
    return GpibProviderRegistration(id);
}

std::vector<std::string> discover_gpib_resources() {
    std::vector<std::string> resources;
    for (const auto& provider : provider_snapshot()) {
        try {
            for (const auto& name : provider->discover()) {
                const auto parsed = parse_resource(name);
                if (parsed && (parsed->kind == ResourceKind::gpib_instr ||
                               parsed->kind == ResourceKind::gpib_intfc)) {
                    resources.push_back(parsed->canonical_name);
                }
            }
        } catch (...) {
            // Discovery is best-effort. One controller must not hide resources
            // already reported by another provider.
        }
    }
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()),
                    resources.end());
    return resources;
}

std::unique_ptr<GpibTransport> open_gpib_transport(
    const ResourceDescriptor& resource, ViUInt32 timeout, ViStatus& status) {
    const auto providers = provider_snapshot();
    if (providers.empty()) {
        status = VI_ERROR_NSUP_OPER;
        return nullptr;
    }
    ViStatus deferred_status = VI_ERROR_RSRC_NFOUND;
    for (const auto& provider : providers) {
        status = VI_ERROR_RSRC_NFOUND;
        auto transport = provider->open(resource, timeout, status);
        if (transport) {
            if (status < VI_SUCCESS) {
                return nullptr;
            }
            return transport;
        }
        if (status >= VI_SUCCESS) {
            status = VI_ERROR_SYSTEM_ERROR;
            return nullptr;
        }
        if (status != VI_ERROR_RSRC_NFOUND &&
            deferred_status == VI_ERROR_RSRC_NFOUND) {
            deferred_status = status;
        }
    }
    status = deferred_status;
    return nullptr;
}

}  // namespace wrvisa
