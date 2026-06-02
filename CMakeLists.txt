cmake_minimum_required(VERSION 3.28)

project(hand_sdk_cpp VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOEM_ROOT "$ENV{HOME}/Documents/tools/SOEM")
set(SOEM_BUILD "${SOEM_ROOT}/build/default")

add_executable(hand_sdk_cpp
    src/main.cpp
)

target_include_directories(hand_sdk_cpp PRIVATE
    ${SOEM_ROOT}/include
    ${SOEM_BUILD}/include
    ${SOEM_ROOT}/osal
    ${SOEM_ROOT}/osal/linux
    ${SOEM_ROOT}/oshw/linux
)

target_link_libraries(hand_sdk_cpp PRIVATE
    ${SOEM_BUILD}/libsoem.a
    pthread
    rt
)