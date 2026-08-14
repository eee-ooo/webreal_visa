#ifndef WRVISA_BACKENDS_GPIB_GPIB_PROVIDER_H
#define WRVISA_BACKENDS_GPIB_GPIB_PROVIDER_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "backends/gpib/gpib_transport.h"
#include "resource/resource_parser.h"

namespace wrvisa {

class GpibProvider {
public:
    virtual ~GpibProvider() = default;

    virtual std::vector<std::string> discover() = 0;
    virtual std::unique_ptr<GpibTransport> open(
        const ResourceDescriptor& resource, ViUInt32 timeout,
        ViStatus& status) = 0;
};

class GpibProviderRegistration {
public:
    GpibProviderRegistration() = default;
    GpibProviderRegistration(const GpibProviderRegistration&) = delete;
    GpibProviderRegistration& operator=(const GpibProviderRegistration&) = delete;
    GpibProviderRegistration(GpibProviderRegistration&& other) noexcept;
    GpibProviderRegistration& operator=(
        GpibProviderRegistration&& other) noexcept;
    ~GpibProviderRegistration();

    explicit operator bool() const noexcept { return id_ != 0; }
    void reset() noexcept;

private:
    friend GpibProviderRegistration register_gpib_provider(
        std::shared_ptr<GpibProvider> provider);
    explicit GpibProviderRegistration(std::uint64_t id) : id_(id) {}

    std::uint64_t id_{0};
};

GpibProviderRegistration register_gpib_provider(
    std::shared_ptr<GpibProvider> provider);
std::vector<std::string> discover_gpib_resources();
std::unique_ptr<GpibTransport> open_gpib_transport(
    const ResourceDescriptor& resource, ViUInt32 timeout, ViStatus& status);

}  // namespace wrvisa

#endif
