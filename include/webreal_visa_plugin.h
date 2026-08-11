#ifndef WEBREAL_VISA_PLUGIN_H
#define WEBREAL_VISA_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#include "visa.h"

#define WRVISA_PLUGIN_ABI_MAJOR 1u
#define WRVISA_PLUGIN_ABI_MINOR 0u

typedef struct wrvisa_host_services_v1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t reserved;
    void* host_context;
    void (WRVISA_CALL* log_message)(void* host_context, uint32_t level,
                                    const char* message);
} wrvisa_host_services_v1;

typedef struct wrvisa_plugin_descriptor_v1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t reserved;
    uint64_t capabilities;
    /* UTF-8 strings remain owned by the loaded plugin until process shutdown. */
    const char* plugin_name;
    const char* plugin_version;
    /* Capability-specific table whose first member must be uint32_t struct_size. */
    void* extension_table;
} wrvisa_plugin_descriptor_v1;

typedef ViStatus (WRVISA_CALL* wrvisa_plugin_query_fn)(
    const wrvisa_host_services_v1* host,
    wrvisa_plugin_descriptor_v1* descriptor);

#endif
