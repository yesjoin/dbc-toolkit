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
add_executable(dbc-dump tools/dbc_dump.cpp src/Dbc.cpp)
target_include_directories(dbc-dump PRIVATE include)
