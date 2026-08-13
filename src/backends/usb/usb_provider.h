#ifndef WRVISA_BACKENDS_USB_USB_PROVIDER_H
#define WRVISA_BACKENDS_USB_USB_PROVIDER_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "backends/usb/usb_transport.h"
#include "resource/resource_parser.h"

namespace wrvisa {

class UsbProvider {
public:
    virtual ~UsbProvider() = default;

    virtual std::vector<std::string> discover() = 0;
    virtual std::unique_ptr<UsbTransport> open(
        const ResourceDescriptor& resource, ViUInt32 timeout,
        ViStatus& status) = 0;
    virtual std::unique_ptr<UsbTransport> open_raw(
        const ResourceDescriptor& resource,
        const UsbRawConfiguration& configuration, ViUInt32 timeout,
        ViStatus& status) {
        static_cast<void>(resource);
        static_cast<void>(configuration);
        static_cast<void>(timeout);
        status = VI_ERROR_RSRC_NFOUND;
        return nullptr;
    }
};

class UsbProviderRegistration {
public:
    UsbProviderRegistration() = default;
    UsbProviderRegistration(const UsbProviderRegistration&) = delete;
    UsbProviderRegistration& operator=(const UsbProviderRegistration&) = delete;
    UsbProviderRegistration(UsbProviderRegistration&& other) noexcept;
    UsbProviderRegistration& operator=(UsbProviderRegistration&& other) noexcept;
    ~UsbProviderRegistration();

    explicit operator bool() const noexcept { return id_ != 0; }
    void reset() noexcept;

private:
    friend UsbProviderRegistration register_usb_provider(
        std::shared_ptr<UsbProvider> provider);
    explicit UsbProviderRegistration(std::uint64_t id) : id_(id) {}

    std::uint64_t id_{0};
};

UsbProviderRegistration register_usb_provider(
    std::shared_ptr<UsbProvider> provider);
std::vector<std::string> discover_usb_resources();
std::unique_ptr<UsbTransport> open_usb_transport(
    const ResourceDescriptor& resource, ViUInt32 timeout, ViStatus& status);
std::unique_ptr<UsbTransport> open_usb_raw_transport(
    const ResourceDescriptor& resource,
    const UsbRawConfiguration& configuration, ViUInt32 timeout,
    ViStatus& status);

class UsbInterfaceLease {
public:
    explicit UsbInterfaceLease(std::function<void()> release)
        : release_(std::move(release)) {}
    UsbInterfaceLease(const UsbInterfaceLease&) = delete;
    UsbInterfaceLease& operator=(const UsbInterfaceLease&) = delete;
    ~UsbInterfaceLease();

    void invalidate() noexcept;

private:
    std::mutex mutex_;
    std::function<void()> release_;
};

class UsbInterfaceArbiter {
public:
    std::shared_ptr<UsbInterfaceLease> acquire(
        const std::string& identity, std::function<ViStatus()> claim,
        std::function<void()> release, ViStatus& status);
    void invalidate(const std::string& identity) noexcept;
    void invalidate_all() noexcept;

private:
    std::mutex mutex_;
    std::map<std::string, std::weak_ptr<UsbInterfaceLease>> leases_;
};

}  // namespace wrvisa

#endif
