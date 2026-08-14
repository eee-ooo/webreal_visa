cmake_minimum_required(VERSION 3.25)

project(webreal_visa VERSION 0.6.0 LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)
find_package(Threads REQUIRED)

option(WRVISA_BUILD_SHARED "Build the shared webreal_visa library" ON)
option(WRVISA_BUILD_STATIC "Build the static webreal_visa library" ON)
set(WRVISA_LIBUSB "AUTO" CACHE STRING
    "Build the production libusb transport: AUTO, ON, or OFF")
set_property(CACHE WRVISA_LIBUSB PROPERTY STRINGS AUTO ON OFF)
set(WRVISA_ASIO_SOURCE_DIR "" CACHE PATH
    "Existing Asio source tree (the directory containing asio/include/asio.hpp)")

if(NOT WRVISA_BUILD_SHARED AND NOT WRVISA_BUILD_STATIC)
    message(FATAL_ERROR "At least one of WRVISA_BUILD_SHARED/WRVISA_BUILD_STATIC must be ON")
endif()
if(NOT WRVISA_LIBUSB MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "WRVISA_LIBUSB must be AUTO, ON, or OFF")
endif()

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(WRVISA_HAS_LIBUSB OFF)
if(NOT WRVISA_LIBUSB STREQUAL "OFF")
    find_package(LibUSB 1.0.30 QUIET)
    if(LibUSB_FOUND)
        set(WRVISA_HAS_LIBUSB ON)
        message(STATUS "webreal_visa: production libusb transport enabled")
    elseif(WRVISA_LIBUSB STREQUAL "ON")
        message(FATAL_ERROR
            "WRVISA_LIBUSB=ON requires libusb 1.0.30 headers and dynamic library")
    else()
        message(STATUS
            "webreal_visa: libusb 1.0.30 not found; production USB transport disabled")
    endif()
endif()

if(WRVISA_ASIO_SOURCE_DIR)
    set(WRVISA_ASIO_INCLUDE_DIR "${WRVISA_ASIO_SOURCE_DIR}/asio/include")
else()
    FetchContent_Declare(wrvisa_asio
        URL https://codeload.github.com/chriskohlhoff/asio/tar.gz/8806a6803cde7054c3049d3666d3ec36786568c5
        URL_HASH SHA256=ca7f6c14f2bf91e61c7e81fb693f2f8fc86f93e85520d5fc7fd035d0f666bb35
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(wrvisa_asio)
    set(WRVISA_ASIO_INCLUDE_DIR "${wrvisa_asio_SOURCE_DIR}/asio/include")
endif()
if(NOT EXISTS "${WRVISA_ASIO_INCLUDE_DIR}/asio.hpp")
    message(FATAL_ERROR "WRVISA_ASIO_SOURCE_DIR does not contain asio/include/asio.hpp")
endif()

set(WRVISA_SOURCES
    src/api/visa_api.cpp
    src/core/handle_table.cpp
    src/core/objects.cpp
    src/core/lock_manager.cpp
    src/resource/resource_parser.cpp
    src/resource/find_expression.cpp
    src/runtime/io_runtime.cpp
    src/runtime/operation.cpp
    src/backends/asio/request_channel.cpp
    src/backends/gpib/gpib_provider.cpp
    src/backends/gpib/gpib_session.cpp
    src/backends/mock/mock_session.cpp
    src/backends/hislip/hislip_protocol.cpp
    src/backends/hislip/hislip_session.cpp
    src/backends/tcp/tcp_session.cpp
    src/backends/usb/libusb_provider.cpp
    src/backends/usb/usb_raw_session.cpp
    src/backends/usb/usb_provider.cpp
    src/backends/usb/usbtmc_protocol.cpp
    src/backends/usb/usbtmc_session.cpp
    src/backends/vxi11/vxi11_protocol.cpp
    src/backends/vxi11/vxi11_session.cpp
    src/backends/serial/serial_session.cpp
    src/platform/serial_discovery.cpp)

add_library(webreal_visa_objects OBJECT ${WRVISA_SOURCES})
target_compile_features(webreal_visa_objects PUBLIC cxx_std_20)
target_compile_definitions(webreal_visa_objects PRIVATE
    WRVISA_BUILDING_LIBRARY
    WRVISA_HAS_LIBUSB=$<BOOL:${WRVISA_HAS_LIBUSB}>
    ASIO_STANDALONE
    ASIO_NO_DEPRECATED)
target_include_directories(webreal_visa_objects
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(webreal_visa_objects SYSTEM PRIVATE
    ${WRVISA_ASIO_INCLUDE_DIR})
if(WRVISA_HAS_LIBUSB)
    target_link_libraries(webreal_visa_objects PRIVATE LibUSB::LibUSB)
endif()
set_target_properties(webreal_visa_objects PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)

if(MSVC)
    target_compile_options(webreal_visa_objects PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(webreal_visa_objects PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

if(WRVISA_BUILD_SHARED)
    add_library(webreal_visa_shared SHARED $<TARGET_OBJECTS:webreal_visa_objects>)
    add_library(webreal_visa::visa ALIAS webreal_visa_shared)
    target_link_libraries(webreal_visa_shared PRIVATE Threads::Threads)
    if(WRVISA_HAS_LIBUSB)
        # Keep libusb headers out of the public compile interface while making
        # the exact dynamic dependency available to installed shared-library
        # consumers. This also prevents an older system libusb with the same
        # SONAME from being selected while linking a consumer executable.
        target_link_libraries(webreal_visa_shared PUBLIC
            "$<LINK_ONLY:LibUSB::LibUSB>")
    endif()
    if(WIN32)
        target_link_libraries(webreal_visa_shared PRIVATE ws2_32 mswsock)
    endif()
    target_include_directories(webreal_visa_shared PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_definitions(webreal_visa_shared INTERFACE WRVISA_SHARED_LIBRARY)
    set_target_properties(webreal_visa_shared PROPERTIES
        EXPORT_NAME visa
        OUTPUT_NAME webreal_visa
        VERSION ${PROJECT_VERSION}
        SOVERSION 0)
    if(UNIX AND NOT APPLE)
        target_link_options(webreal_visa_shared PRIVATE
            "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/webreal_visa.map")
    endif()
endif()

if(WRVISA_BUILD_STATIC)
    add_library(webreal_visa_static STATIC $<TARGET_OBJECTS:webreal_visa_objects>)
    add_library(webreal_visa::visa_static ALIAS webreal_visa_static)
    target_link_libraries(webreal_visa_static PUBLIC Threads::Threads)
    if(WRVISA_HAS_LIBUSB)
        target_link_libraries(webreal_visa_static PRIVATE LibUSB::LibUSB)
    endif()
    if(WIN32)
        target_link_libraries(webreal_visa_static PUBLIC ws2_32 mswsock)
    endif()
    target_include_directories(webreal_visa_static PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    set_target_properties(webreal_visa_static PROPERTIES
        EXPORT_NAME visa_static
        OUTPUT_NAME webreal_visa_static)
endif()

if(BUILD_TESTING)
    if(NOT WRVISA_BUILD_STATIC)
        message(FATAL_ERROR "BUILD_TESTING requires WRVISA_BUILD_STATIC=ON")
    endif()

    function(wrvisa_add_cpp_test target source)
        add_executable(${target} ${source})
        target_compile_features(${target} PRIVATE cxx_std_20)
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(${target} PRIVATE webreal_visa::visa_static)
        add_test(NAME ${target} COMMAND ${target})
    endfunction()

    wrvisa_add_cpp_test(resource_tests tests/resource_tests.cpp)
    wrvisa_add_cpp_test(core_tests tests/core_tests.cpp)
    wrvisa_add_cpp_test(api_tests tests/api_tests.cpp)
    wrvisa_add_cpp_test(concurrency_tests tests/concurrency_tests.cpp)
    wrvisa_add_cpp_test(tcp_tests tests/tcp_tests.cpp)
    target_compile_definitions(tcp_tests PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
    target_include_directories(tcp_tests SYSTEM PRIVATE ${WRVISA_ASIO_INCLUDE_DIR})
    wrvisa_add_cpp_test(protocol_codec_tests tests/protocol_codec_tests.cpp)
    wrvisa_add_cpp_test(gpib_tests tests/gpib_tests.cpp)
    wrvisa_add_cpp_test(usb_tests tests/usb_tests.cpp)
    wrvisa_add_cpp_test(usb_raw_tests tests/usb_raw_tests.cpp)
    wrvisa_add_cpp_test(usb_api_tests tests/usb_api_tests.cpp)
    wrvisa_add_cpp_test(libusb_provider_tests tests/libusb_provider_tests.cpp)
    if(WRVISA_HAS_LIBUSB)
        wrvisa_add_cpp_test(libusb_adapter_tests tests/libusb_adapter_tests.cpp)
        target_link_libraries(libusb_adapter_tests PRIVATE LibUSB::LibUSB)
    endif()
    wrvisa_add_cpp_test(hislip_tests tests/hislip_tests.cpp)
    target_compile_definitions(hislip_tests PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
    target_include_directories(hislip_tests SYSTEM PRIVATE ${WRVISA_ASIO_INCLUDE_DIR})
    wrvisa_add_cpp_test(vxi11_tests tests/vxi11_tests.cpp)
    target_compile_definitions(vxi11_tests PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
    target_include_directories(vxi11_tests SYSTEM PRIVATE ${WRVISA_ASIO_INCLUDE_DIR})
    if(UNIX)
        wrvisa_add_cpp_test(serial_tests tests/serial_tests.cpp)
    endif()

    add_executable(c_header_smoke tests/c_header_smoke.c)
    set_target_properties(c_header_smoke PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
    if(WRVISA_BUILD_SHARED)
        target_link_libraries(c_header_smoke PRIVATE webreal_visa::visa)
    else()
        target_link_libraries(c_header_smoke PRIVATE webreal_visa::visa_static)
    endif()
    add_test(NAME c_header_smoke COMMAND c_header_smoke)

    add_executable(cpp_header_smoke tests/cpp_header_smoke.cpp)
    target_compile_features(cpp_header_smoke PRIVATE cxx_std_20)
    target_link_libraries(cpp_header_smoke PRIVATE webreal_visa::visa_static)
    add_test(NAME cpp_header_smoke COMMAND cpp_header_smoke)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME docs_consistency
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_docs.py)
        set_tests_properties(docs_consistency PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME abi_history
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_abi_history.py)
        if(WRVISA_BUILD_SHARED)
            add_test(NAME abi_exports
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_exports.py
                        $<TARGET_FILE:webreal_visa_shared>)
        endif()
    endif()
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/webreal_visa)
install(FILES third_party/asio/LICENSE_1_0.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/webreal_visa/licenses/asio)
install(FILES third_party/libusb/COPYING
    DESTINATION ${CMAKE_INSTALL_DATADIR}/webreal_visa/licenses/libusb)
if(WRVISA_BUILD_SHARED)
    install(TARGETS webreal_visa_shared EXPORT webreal_visaTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
if(WRVISA_BUILD_STATIC)
    install(TARGETS webreal_visa_static EXPORT webreal_visaTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

set(WRVISA_PACKAGE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/webreal_visa)
configure_package_config_file(
    cmake/webreal_visaConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/webreal_visaConfig.cmake
    INSTALL_DESTINATION ${WRVISA_PACKAGE_DIR})
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/webreal_visaConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(EXPORT webreal_visaTargets
    FILE webreal_visaTargets.cmake
    NAMESPACE webreal_visa::
    DESTINATION ${WRVISA_PACKAGE_DIR})
install(FILES
    cmake/FindLibUSB.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/webreal_visaConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/webreal_visaConfigVersion.cmake
    DESTINATION ${WRVISA_PACKAGE_DIR})

configure_file(cmake/uninstall.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake @ONLY)
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake)
