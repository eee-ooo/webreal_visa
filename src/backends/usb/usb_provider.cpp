#include "backends/usb/usb_provider.h"

#include <algorithm>
#include <map>
#include <utility>

#include "backends/usb/libusb_provider.h"

namespace wrvisa {
namespace {

struct RegistryState {
    std::mutex mutex;
    std::uint64_t next_id{1};
    std::map<std::uint64_t, std::shared_ptr<UsbProvider>> providers;
};

RegistryState& registry() {
    static auto* state = new RegistryState();
    return *state;
}

std::vector<std::shared_ptr<UsbProvider>> provider_snapshot() {
    auto& state = registry();
    std::lock_guard lock(state.mutex);
    std::vector<std::shared_ptr<UsbProvider>> result;
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

UsbProviderRegistration::UsbProviderRegistration(
    UsbProviderRegistration&& other) noexcept
    : id_(std::exchange(other.id_, 0)) {}

UsbProviderRegistration& UsbProviderRegistration::operator=(
    UsbProviderRegistration&& other) noexcept {
    if (this != &other) {
        reset();
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

UsbProviderRegistration::~UsbProviderRegistration() { reset(); }

void UsbProviderRegistration::reset() noexcept {
    if (id_ != 0) {
        unregister_provider(std::exchange(id_, 0));
    }
}

UsbProviderRegistration register_usb_provider(
    std::shared_ptr<UsbProvider> provider) {
    if (!provider) {
        return {};
    }
    auto& state = registry();
    std::lock_guard lock(state.mutex);
    const auto id = state.next_id++;
    state.providers.emplace(id, std::move(provider));
    return UsbProviderRegistration(id);
}

std::vector<std::string> discover_usb_resources() {
    ensure_libusb_provider_registered();
    std::vector<std::string> resources;
    for (const auto& provider : provider_snapshot()) {
        try {
            for (const auto& name : provider->discover()) {
                const auto parsed = parse_resource(name);
                if (parsed && (parsed->kind == ResourceKind::usb_instr ||
                               parsed->kind == ResourceKind::usb_raw)) {
                    resources.push_back(parsed->canonical_name);
                }
            }
        } catch (...) {
            // Discovery is best-effort. A failing provider must not hide the
            // resources already reported by other transports.
        }
    }
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()),
                    resources.end());
    return resources;
}

std::unique_ptr<UsbTransport> open_usb_transport(
    const ResourceDescriptor& resource, ViUInt32 timeout, ViStatus& status) {
    ensure_libusb_provider_registered();
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

std::unique_ptr<UsbTransport> open_usb_raw_transport(
    const ResourceDescriptor& resource,
    const UsbRawConfiguration& configuration, ViUInt32 timeout,
    ViStatus& status) {
    ensure_libusb_provider_registered();
    const auto providers = provider_snapshot();
    if (providers.empty()) {
        status = VI_ERROR_NSUP_OPER;
        return nullptr;
    }
    ViStatus deferred_status = VI_ERROR_RSRC_NFOUND;
    for (const auto& provider : providers) {
        status = VI_ERROR_RSRC_NFOUND;
        auto transport =
            provider->open_raw(resource, configuration, timeout, status);
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

UsbInterfaceLease::~UsbInterfaceLease() { invalidate(); }

void UsbInterfaceLease::invalidate() noexcept {
    std::function<void()> release;
    {
        std::lock_guard lock(mutex_);
        release = std::move(release_);
    }
    if (release) {
        try {
            release();
        } catch (...) {
        }
    }
}

std::shared_ptr<UsbInterfaceLease> UsbInterfaceArbiter::acquire(
    const std::string& identity, std::function<ViStatus()> claim,
    std::function<void()> release, ViStatus& status) {
    if (identity.empty() || !claim || !release) {
        status = VI_ERROR_INV_PARAMETER;
        return nullptr;
    }
    std::lock_guard lock(mutex_);
    if (const auto found = leases_.find(identity); found != leases_.end()) {
        if (auto lease = found->second.lock()) {
            status = VI_SUCCESS;
            return lease;
        }
        leases_.erase(found);
    }
    status = claim();
    if (status < VI_SUCCESS) {
        return nullptr;
    }
    auto lease = std::make_shared<UsbInterfaceLease>(std::move(release));
    leases_[identity] = lease;
    return lease;
}

void UsbInterfaceArbiter::invalidate(const std::string& identity) noexcept {
    std::shared_ptr<UsbInterfaceLease> lease;
    {
        std::lock_guard lock(mutex_);
        const auto found = leases_.find(identity);
        if (found == leases_.end()) {
            return;
        }
        lease = found->second.lock();
        leases_.erase(found);
    }
    if (lease) {
        lease->invalidate();
    }
}

void UsbInterfaceArbiter::invalidate_all() noexcept {
    std::vector<std::shared_ptr<UsbInterfaceLease>> leases;
    {
        std::lock_guard lock(mutex_);
        leases.reserve(leases_.size());
        for (const auto& [identity, weak_lease] : leases_) {
            static_cast<void>(identity);
            if (auto lease = weak_lease.lock()) {
                leases.push_back(std::move(lease));
            }
        }
        leases_.clear();
    }
    for (const auto& lease : leases) {
        lease->invalidate();
    }
}

}  // namespace wrvisa
