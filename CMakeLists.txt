cmake_minimum_required(VERSION 3.22)
project(archiver
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "Fast archiver optimized for network filesystems"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# crc32c 1.1.2 declares cmake_minimum_required(VERSION 3.1), rejected by CMake >= 4
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

option(ARCH_WITH_URING "Build the optional io_uring backend (requires liburing)" OFF)

include(FetchContent)

# ---------- dependencies (all via FetchContent) ----------

# spdlog (bundles fmt as submodule, transitively provides fmt targets)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)

# zstd (cache vars MUST be set before MakeAvailable)
set(ZSTD_BUILD_PROGRAMS   OFF  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED     OFF  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC     ON   CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)

# crc32c
FetchContent_Declare(crc32c
    GIT_REPOSITORY https://github.com/google/crc32c.git
    GIT_TAG 1.1.2
    GIT_SHALLOW TRUE
)
set(CRC32C_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(CRC32C_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(CRC32C_USE_GNU_DOWNLOADS_PAGE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(crc32c)

# xxHash
FetchContent_Declare(xxHash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG v0.8.3
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR cmake_unofficial
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(xxHash)

# ---------- library ----------
set(ARCHIVER_SOURCES
    src/compress/compressor.cpp
    src/compress/zstd_impl.cpp
    src/archive/writer.cpp
    src/util/xattr.cpp
)

if(ARCH_WITH_URING)
    # liburing – system library (requires liburing-devel / liburing-dev)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)
    list(APPEND ARCHIVER_SOURCES
        src/io/io_uring_pool.cpp
        src/io/prefetch.cpp
    )
endif()

add_library(archiver_lib STATIC ${ARCHIVER_SOURCES})

target_include_directories(archiver_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(archiver_lib PUBLIC
    spdlog::spdlog
    libzstd_static
    crc32c
    xxHash::xxhash
)

if(ARCH_WITH_URING)
    target_compile_definitions(archiver_lib PUBLIC ARCH_WITH_URING)
    target_link_libraries(archiver_lib PUBLIC PkgConfig::LIBURING)
endif()

target_compile_options(archiver_lib PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-c++98-compat>
    $<$<CXX_COMPILER_ID:GNU>:-fdiagnostics-color=always>
)

# ---------- CLI tool ----------
add_executable(arch src/main.cpp)
target_link_libraries(arch PRIVATE archiver_lib spdlog::spdlog)

# Catch2 (v3)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.9.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

# catch_discover_tests() lives in Catch2's extras/ module dir, which is not
# propagated to the parent scope by FetchContent — add it manually.
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ---------- tests ----------
enable_testing()
add_subdirectory(test)
