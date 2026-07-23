# ============================================================================
# cppcoro 教学构建 (原版只有 cake 构建, 此为简化的 CMake 版)
#   - libcppcoro: lib/*.cpp 编出的静态库 (剔除 Linux 下不可用的平台代码)
#   - demo:       教学示例 (task 链 / when_all 并发 / 跨线程唤醒 / 线程池调度)
# 用法: cmake -B build && cmake --build build -j && ./build/demo
#
# 【Linux 剔除清单】上游 cppcoro 的 Linux 支持为 WIP (见 README):
#   - io_service 只有 Windows IOCP 实现 (无 epoll), Linux 下是空壳 → 剔除
#   - socket/net 依赖 io_service 的唤醒机制 → 剔除
#   - file 系列只有 Windows 实现 (守卫都不完整, 编译即错) → 剔除
# 保留的部分 (task/when_all/同步原语/线程池/取消) 全平台可编译运行。
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(cppcoro_learn CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GCC 需要 -fcoroutines 才启用协程; clang 不需要
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fcoroutines)
endif()

file(GLOB CPPCORO_SOURCES lib/*.cpp)

# Linux 下不可编译的平台专属源码 (详见文件头说明)
list(REMOVE_ITEM CPPCORO_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/io_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/file.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/file_read_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/file_write_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/read_only_file.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/read_write_file.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/readable_file.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/writable_file.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/write_only_file.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/socket.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/socket_accept_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/socket_connect_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/socket_disconnect_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/socket_helpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/socket_recv_from_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/socket_recv_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/socket_send_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/socket_send_to_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/win32.cpp
)

add_library(cppcoro STATIC ${CPPCORO_SOURCES})
target_include_directories(cppcoro PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include   # 公开头 + experimental shim
    ${CMAKE_CURRENT_SOURCE_DIR}/lib)      # 内部头 (spin_mutex 等)
target_link_libraries(cppcoro PUBLIC pthread)

add_executable(demo demo.cpp)
target_link_libraries(demo PRIVATE cppcoro)
