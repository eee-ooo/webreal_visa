# Locate libusb 1.0 without exposing it through webreal_visa public headers.

find_path(LibUSB_INCLUDE_DIR
    NAMES libusb.h
    PATH_SUFFIXES libusb-1.0)
find_library(LibUSB_LIBRARY
    NAMES usb-1.0 libusb-1.0)
if(WIN32)
    find_file(LibUSB_RUNTIME_LIBRARY
        NAMES libusb-1.0.dll
        PATH_SUFFIXES bin)
elseif(LibUSB_LIBRARY MATCHES "\\.(a|lib)$")
    # This project deliberately uses libusb as an LGPL dynamic boundary.
    # Do not silently accept a static archive when it is the only candidate.
    set(LibUSB_LIBRARY "LibUSB_LIBRARY-NOTFOUND")
endif()

set(LibUSB_VERSION "")
if(LibUSB_INCLUDE_DIR AND EXISTS "${LibUSB_INCLUDE_DIR}/libusb.h")
    file(STRINGS "${LibUSB_INCLUDE_DIR}/libusb.h" _libusb_api_line
        REGEX "^#define[ \t]+LIBUSB_API_VERSION[ \t]+0x[0-9A-Fa-f]+")
    if(_libusb_api_line MATCHES "(0x[0-9A-Fa-f]+)")
        math(EXPR _libusb_api_version "${CMAKE_MATCH_1}")
        if(_libusb_api_version GREATER_EQUAL 16777484) # 0x0100010C
            set(LibUSB_VERSION "1.0.30")
        endif()
    endif()
endif()

include(FindPackageHandleStandardArgs)
set(_LibUSB_REQUIRED_VARS LibUSB_INCLUDE_DIR LibUSB_LIBRARY)
if(WIN32)
    list(APPEND _LibUSB_REQUIRED_VARS LibUSB_RUNTIME_LIBRARY)
endif()
find_package_handle_standard_args(LibUSB
    REQUIRED_VARS ${_LibUSB_REQUIRED_VARS}
    VERSION_VAR LibUSB_VERSION)

if(LibUSB_FOUND AND NOT TARGET LibUSB::LibUSB)
    add_library(LibUSB::LibUSB SHARED IMPORTED)
    if(WIN32)
        set_target_properties(LibUSB::LibUSB PROPERTIES
            IMPORTED_IMPLIB "${LibUSB_LIBRARY}"
            IMPORTED_LOCATION "${LibUSB_RUNTIME_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LibUSB_INCLUDE_DIR}")
    else()
        set_target_properties(LibUSB::LibUSB PROPERTIES
            IMPORTED_LOCATION "${LibUSB_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LibUSB_INCLUDE_DIR}")
    endif()
endif()

mark_as_advanced(LibUSB_INCLUDE_DIR LibUSB_LIBRARY LibUSB_RUNTIME_LIBRARY)
