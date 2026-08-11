#include <type_traits>

#include "visa.h"
#include "webreal_visa_ext.h"
#include "webreal_visa_plugin.h"

static_assert(std::is_standard_layout_v<wrvisa_host_services_v1>);
static_assert(std::is_standard_layout_v<wrvisa_plugin_descriptor_v1>);
static_assert(sizeof(ViUInt32) == 4);
static_assert(sizeof(ViSession) == 4);
static_assert(VI_SPEC_VERSION == 0x00700200u);

int main() {
    ViSession session = VI_NULL;
    return viOpenDefaultRM(&session) == VI_SUCCESS && viClose(session) == VI_SUCCESS ? 0 : 1;
}
