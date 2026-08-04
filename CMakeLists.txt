cmake_minimum_required(VERSION 3.28)

project(hand_sdk_cpp VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Where SOEM lives. Override without editing this file:
#   cmake -B build -DSOEM_ROOT=/path/to/SOEM
# or set the SOEM_ROOT environment variable.
if(NOT SOEM_ROOT)
    if(DEFINED ENV{SOEM_ROOT})
        set(SOEM_ROOT "$ENV{SOEM_ROOT}")
    else()
        set(SOEM_ROOT "$ENV{HOME}/Documents/tools/SOEM")
    endif()
endif()
set(SOEM_ROOT "${SOEM_ROOT}" CACHE PATH "Root of the SOEM source tree")
set(SOEM_BUILD "${SOEM_ROOT}/build/default" CACHE PATH "SOEM build directory")

if(NOT EXISTS "${SOEM_BUILD}/libsoem.a")
    message(FATAL_ERROR
        "SOEM not found: ${SOEM_BUILD}/libsoem.a\n"
        "Build SOEM first, then point this package at it:\n"
        "  cmake -S <soem> -B <soem>/build/default && cmake --build <soem>/build/default\n"
        "  cmake -B build -DSOEM_ROOT=<soem>\n"
        "See README.md, 'Prerequisites'.")
endif()

set(SOEM_INCLUDES
    ${SOEM_ROOT}/include
    ${SOEM_BUILD}/include
    ${SOEM_ROOT}/osal
    ${SOEM_ROOT}/osal/linux
    ${SOEM_ROOT}/oshw/linux
)

# --- SDK ---------------------------------------------------------------------
# SOEM is a PRIVATE dependency: it is needed to build the library, but consumers
# only ever see include/, which has no EtherCAT headers in it. That is what lets
# a ROS node link this without knowing SOEM exists.
add_library(hand_sdk STATIC
    src/api.cpp
    src/cyclic_task.cpp
    src/realtime.cpp
    src/connections/ethercat_connection.cpp
)

add_library(hand_sdk::hand_sdk ALIAS hand_sdk)

target_include_directories(hand_sdk
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SOEM_INCLUDES}
)

target_link_libraries(hand_sdk
    PRIVATE ${SOEM_BUILD}/libsoem.a
    PUBLIC pthread rt
)

# --- Example -----------------------------------------------------------------
# User-layer usage: links hand_sdk only, no SOEM include paths.
add_executable(hand_sdk_example examples/main.cpp)
target_link_libraries(hand_sdk_example PRIVATE hand_sdk)

# --- Diagnostics -------------------------------------------------------------
# Standalone bring-up tools. They talk to SOEM directly rather than through the
# SDK, on purpose: they are for inspecting a bus whose configuration is still
# unknown, which is the situation the SDK's assumptions are derived from.

# Reads a simple bit-IO slave; no hand-specific PDO layout involved.
add_executable(bit_io_test examples/bit_io_test.cpp)
target_include_directories(bit_io_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SOEM_INCLUDES})
target_link_libraries(bit_io_test PRIVATE ${SOEM_BUILD}/libsoem.a pthread rt)

# --- Install / export --------------------------------------------------------
# Consumers use:
#   find_package(hand_sdk REQUIRED)
#   target_link_libraries(app PRIVATE hand_sdk::hand_sdk)
# The installed archive bundles SOEM's objects (ar MRI script below), so
# consumers need neither a SOEM checkout nor its include paths — only the
# public headers and pthread/rt. Nothing in src/ is installed.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(BUNDLED_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/libhand_sdk_bundled.a)
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bundle_soem.mri CONTENT
"create ${BUNDLED_ARCHIVE}
addlib $<TARGET_FILE:hand_sdk>
addlib ${SOEM_BUILD}/libsoem.a
save
end
")
add_custom_command(TARGET hand_sdk POST_BUILD
    COMMAND sh -c "${CMAKE_AR} -M < ${CMAKE_CURRENT_BINARY_DIR}/bundle_soem.mri"
    COMMENT "Bundling SOEM into libhand_sdk_bundled.a"
    VERBATIM)

install(FILES ${BUNDLED_ARCHIVE}
        DESTINATION ${CMAKE_INSTALL_LIBDIR} RENAME libhand_sdk.a)
install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hand_sdk)

configure_package_config_file(cmake/hand_sdkConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/hand_sdkConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hand_sdk
    PATH_VARS CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_INCLUDEDIR)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/hand_sdkConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hand_sdkConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/hand_sdkConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hand_sdk)
