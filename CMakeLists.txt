cmake_minimum_required(VERSION 3.15.2)

project(ForgeSched LANGUAGES CXX)

# =====================
# C++ 标准
# =====================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =====================
# 构建类型（默认 Debug）
# =====================
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# =====================
# 输出路径
# =====================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Parallelism is controlled by make PARALLEL_JOBS or cmake --build --parallel.
# CMAKE_BUILD_PARALLEL_LEVEL is an environment variable, not a cache variable.

# =====================
# Debug 编译选项
# =====================
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-g)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3 -DNDEBUG)
endif()

# =====================
# 子模块
# =====================
add_subdirectory(common)
add_subdirectory(server)
add_subdirectory(client)
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
