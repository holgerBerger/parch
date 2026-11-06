include(CTest)

# ---------- Catch2 test binary ----------
add_executable(arch_test
    test_format.cpp
    test_compressor.cpp
    test_io_uring.cpp
    test_prefetch.cpp
    test_archive_writer.cpp
    test_xattr.cpp
    test_crc.cpp
    test_endian.cpp
    test_dedup_index.cpp
    test_footer_serialize.cpp
    test_toc_serialize.cpp
)

target_link_libraries(arch_test PRIVATE
    archiver_lib
    spdlog::spdlog_headerless
    Catch2::Catch2
)

target_compile_options(arch_test PRIVATE
    -Wall -Wextra
)

catch_discover_tests(arch_test)
