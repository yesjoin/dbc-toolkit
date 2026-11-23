cmake_minimum_required(VERSION 3.16)
project(dbc_toolkit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

# M0：DBC 解析器自测。样例文件用编译期绝对路径传进去，
# 这样从任何目录跑测试都不会因为 cwd 不同而找不到文件。
add_executable(dbc-selftest tests/dbc_selftest.cpp src/Dbc.cpp)
target_include_directories(dbc-selftest PRIVATE include)
target_compile_definitions(dbc-selftest PRIVATE
    DBC_SAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/dbc/sample.dbc")

# M0：把解析结果打成表，M1 调位引擎时的参照物
add_executable(dbc-dump tools/dbc_dump.cpp src/Dbc.cpp src/Bits.cpp src/Mux.cpp src/Codec.cpp)
target_include_directories(dbc-dump PRIVATE include)

# M1：位引擎自测。位号序列、取值、放值、非法声明、抢位检查
add_executable(bits-selftest tests/bits_selftest.cpp src/Dbc.cpp src/Bits.cpp)
target_include_directories(bits-selftest PRIVATE include)
target_compile_definitions(bits-selftest PRIVATE
    DBC_SAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/dbc/sample.dbc")

# M2：编解码自测。符号扩展、换算方向、位宽 vs 量程、值表、编码不碰别人的位
add_executable(codec-selftest tests/codec_selftest.cpp src/Dbc.cpp src/Bits.cpp src/Codec.cpp)
target_include_directories(codec-selftest PRIVATE include)
target_compile_definitions(codec-selftest PRIVATE
    DBC_SAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/dbc/sample.dbc")

# M2：喂一帧 8 字节，把每个信号的值打出来（M3.5 接真帧的雏形）
add_executable(dbc-decode tools/dbc_decode.cpp src/Dbc.cpp src/Bits.cpp src/Codec.cpp src/Mux.cpp)
target_include_directories(dbc-decode PRIVATE include)

# M3：多路复用路由自测。开关识别、分支路由、只写当前分支、按帧查抢位
add_executable(mux-selftest tests/mux_selftest.cpp
    src/Dbc.cpp src/Bits.cpp src/Codec.cpp src/Mux.cpp)
target_include_directories(mux-selftest PRIVATE include)
target_compile_definitions(mux-selftest PRIVATE
    DBC_SAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/dbc/sample.dbc")

# M3.5：真总线收发。dbc-send 按 DBC 编帧发出去，dbc-read 挂总线上解回来。
# CanRaw 只在 Linux 有真实现，别的系统给桩，所以这两个 target 到处都能配。
add_library(canraw STATIC src/CanRaw.cpp)
target_include_directories(canraw PUBLIC include)

add_executable(dbc-send tools/dbc_send.cpp src/Dbc.cpp src/Bits.cpp src/Codec.cpp src/Mux.cpp)
target_include_directories(dbc-send PRIVATE include)
target_link_libraries(dbc-send PRIVATE canraw)

add_executable(dbc-read tools/dbc_read.cpp src/Dbc.cpp src/Bits.cpp src/Codec.cpp src/Mux.cpp)
target_include_directories(dbc-read PRIVATE include)
target_link_libraries(dbc-read PRIVATE canraw)

# ---------------------------------------------------------------------------
# M4：代码生成。dbc-gen 把 DBC 编成自包含的 C++17 头文件，
# 生成物在 build/gen/，由 gen-header 这个 target 负责在构建时产出。
# 注意：生成器不带时间戳，同一份 DBC 永远生成同样的字节，CI 里不会假报变更。
# ---------------------------------------------------------------------------
add_executable(dbc-gen tools/dbc_gen.cpp src/Dbc.cpp src/Bits.cpp src/Codec.cpp src/Mux.cpp)
target_include_directories(dbc-gen PRIVATE include)

set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)

add_custom_command(
    OUTPUT ${GEN_DIR}/sample_dbc.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN_DIR}
    COMMAND $<TARGET_FILE:dbc-gen> ${CMAKE_CURRENT_SOURCE_DIR}/dbc/sample.dbc
            -o ${GEN_DIR}/sample_dbc.h
    COMMENT "dbc-gen: dbc/sample.dbc -> gen/sample_dbc.h"
    DEPENDS dbc-gen ${CMAKE_CURRENT_SOURCE_DIR}/dbc/sample.dbc
    VERBATIM)

add_custom_target(gen-header DEPENDS ${GEN_DIR}/sample_dbc.h)

# M4 验收：差分测试 —— 生成的代码 vs 运行时库，逐字节比
add_executable(gen-selftest tests/gen_selftest.cpp
    src/Dbc.cpp src/Bits.cpp src/Codec.cpp src/Mux.cpp)
add_dependencies(gen-selftest gen-header)
target_include_directories(gen-selftest PRIVATE include ${GEN_DIR})
target_compile_definitions(gen-selftest PRIVATE
    DBC_SAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/dbc/sample.dbc")
