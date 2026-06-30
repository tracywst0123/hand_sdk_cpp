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

# Hand SDK static library
add_library(hand_sdk STATIC
    src/api.cpp
    src/connections/ethercat_connection.cpp
)

target_include_directories(hand_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SOEM_INCLUDES}
)

target_link_libraries(hand_sdk PUBLIC
    ${SOEM_BUILD}/libsoem.a
    pthread
    rt
)

# Example executable
add_executable(hand_sdk_example
    examples/main.cpp
)

target_link_libraries(hand_sdk_example PRIVATE hand_sdk)

# SOEM import test
add_executable(hand_sdk_test
    examples/test.cpp
)

target_include_directories(hand_sdk_test PRIVATE ${SOEM_INCLUDES})
target_link_libraries(hand_sdk_test PRIVATE
    ${SOEM_BUILD}/libsoem.a
    pthread
    rt
)

# Standalone bit-IO read test (simple bit device, no hand-specific PDO layout)
add_executable(bit_io_test
    examples/bit_io_test.cpp
)

target_include_directories(bit_io_test PRIVATE ${SOEM_INCLUDES})
target_link_libraries(bit_io_test PRIVATE
    ${SOEM_BUILD}/libsoem.a
    pthread
    rt
)

# EtherCAT structure dump (SII/EEPROM vs intended PDO sizes diagnostic)
add_executable(ecat_dump
    examples/ecat_dump.cpp
)

target_include_directories(ecat_dump PRIVATE ${SOEM_INCLUDES})
target_link_libraries(ecat_dump PRIVATE
    ${SOEM_BUILD}/libsoem.a
    pthread
    rt
)