cmake_minimum_required(VERSION 3.25)

project(webreal_visa_consumer_example LANGUAGES C CXX)

include(CTest)
find_package(webreal_visa 0.5 CONFIG REQUIRED)

add_executable(mock_query mock_query.c)
set_target_properties(mock_query PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
# A static library implemented in C++ requires the platform C++ runtime even
# though the public source and ABI are C.
set_target_properties(mock_query PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(mock_query PRIVATE webreal_visa::visa_static)

# Exercise the installed shared target separately.  On Windows the imported
# DLL is copied beside the executable so CTest uses the package artifact rather
# than a developer-machine PATH entry.
add_executable(mock_query_shared mock_query.c)
set_target_properties(mock_query_shared PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES)
target_link_libraries(mock_query_shared PRIVATE webreal_visa::visa)
if(WIN32)
    add_custom_command(TARGET mock_query_shared POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:webreal_visa::visa>
                $<TARGET_FILE_DIR:mock_query_shared>)
endif()

add_executable(tcp_query tcp_query.c)
set_target_properties(tcp_query PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    LINKER_LANGUAGE CXX)
target_link_libraries(tcp_query PRIVATE webreal_visa::visa_static)

if(BUILD_TESTING)
    add_test(NAME mock_query COMMAND mock_query)
    add_test(NAME mock_query_shared COMMAND mock_query_shared)
endif()
