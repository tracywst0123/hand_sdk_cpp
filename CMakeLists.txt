cmake_minimum_required(VERSION 3.28)

project(hand_sdk_cpp VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOEM_ROOT "$ENV{HOME}/Documents/tools/SOEM")
set(SOEM_BUILD "${SOEM_ROOT}/build/default")

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

target_include_directories(hand_sdk
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
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

# Dumps SII/EEPROM and live SyncManager registers. This is what established the
# SM2@0x1100 / SM3@0x1400 addresses that ethercat_connection.cpp relies on.
add_executable(ecat_dump examples/ecat_dump.cpp)
target_include_directories(ecat_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SOEM_INCLUDES})
target_link_libraries(ecat_dump PRIVATE ${SOEM_BUILD}/libsoem.a pthread rt)

# Reads a simple bit-IO slave; no hand-specific PDO layout involved.
add_executable(bit_io_test examples/bit_io_test.cpp)
target_include_directories(bit_io_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SOEM_INCLUDES})
target_link_libraries(bit_io_test PRIVATE ${SOEM_BUILD}/libsoem.a pthread rt)
