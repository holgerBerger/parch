include(CTest)

# ---------- Catch2 test binary ----------
set(ARCH_TEST_SOURCES
    test_format.cpp
    test_compressor.cpp
    test_archive_writer.cpp
    test_xattr.cpp
    test_crc.cpp
    test_endian.cpp
    test_dedup_index.cpp
    test_footer_serialize.cpp
    test_toc_serialize.cpp
    test_writer_serialize.cpp
    test_pipeline.cpp
    test_reader.cpp
    test_sparse.cpp
    test_cli.cpp
)

if(ARCH_WITH_URING)
    list(APPEND ARCH_TEST_SOURCES
        test_io_uring.cpp
        test_prefetch.cpp
    )
endif()

add_executable(arch_test ${ARCH_TEST_SOURCES})

# test_cli.cpp runs the `parch` binary as a subprocess.
target_compile_definitions(arch_test PRIVATE
    ARCH_CLI_BINARY="$<TARGET_FILE:parch>")
add_dependencies(arch_test parch)

target_link_libraries(arch_test PRIVATE
    archiver_lib
    spdlog::spdlog
    Catch2::Catch2WithMain
)

target_compile_options(arch_test PRIVATE
    -Wall -Wextra
)

catch_discover_tests(arch_test)
