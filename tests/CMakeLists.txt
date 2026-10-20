
add_executable(slidescape_tests
        test_main.cpp
        test_fixtures.cpp
        test_dicomweb.cpp
        test_archive.cpp
        test_block_allocator.cpp
        test_http_response_parser.cpp
        test_http_request.cpp
        test_mathutils.cpp
        test_source_io.cpp
        test_memrw.cpp
        test_replay.cpp
        test_profiler.cpp
        ../src/utils/profiler.c
        test_remote_url.cpp
        test_stb_sprintf.cpp
        test_slide_score.cpp
        test_ssi.cpp
        test_support.cpp
        test_stringutils.cpp
        test_tile_buffer_pool.cpp
        test_tile_cache.cpp
        test_work_queue.cpp
        ../src/core/slide_score.c
)

target_include_directories(slidescape_tests PRIVATE
        ../src
        ../src/core
        ../src/platform
        ../src/utils
        ../src/third_party
)

target_compile_definitions(slidescape_tests PRIVATE
        USE_CONSOLE_LOGGING=1
        SLIDESCAPE_TEST_FIXTURE_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/manifest.txt"
        SLIDESCAPE_TEST_LOCAL_FIXTURE_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/local_manifest.txt"
        SLIDESCAPE_TEST_DICOMWEB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dicomweb"
        ISYNTAX_JPEG_DECODER_USE_LIBJPEG=1
)
if (WIN32)
        target_compile_definitions(slidescape_tests PRIVATE NOMINMAX) # prevent Windows SDK's min and max macro definitions colliding with std::min and std::max
endif()
target_link_libraries(slidescape_tests PRIVATE slidescape_nongui slidescape_jpeg slidescape_archive_core)
slidescape_enable_process_allocator(slidescape_tests)
if (WIN32)
        target_link_libraries(slidescape_tests PRIVATE winmm psapi)
endif()

add_test(NAME slidescape_tests COMMAND slidescape_tests)

find_package(Python3 QUIET COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
        set(DICOMWEB_TEST_VIEWER slidescape)
        if (WIN32)
                set(DICOMWEB_TEST_VIEWER slidescape_console)
        endif()
        add_test(NAME slidescape_dicomweb_metadata_negotiation
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_dicomweb_metadata_negotiation.py"
                "$<TARGET_FILE:${DICOMWEB_TEST_VIEWER}>")
        set_tests_properties(slidescape_dicomweb_metadata_negotiation PROPERTIES
                TIMEOUT 90 LABELS "dicomweb;loopback")
endif()

if (BUILD_SLIDESCAPE_ARCHIVE)
        if (Python3_Interpreter_FOUND)
                add_test(NAME slidescape_archive_integration
                        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_archive_integration.py"
                        "$<TARGET_FILE:slidescape_archive>")
                set_tests_properties(slidescape_archive_integration PROPERTIES TIMEOUT 90 LABELS "archive;loopback")
        endif()
endif()

add_executable(dicomweb_probe
        dicomweb_probe.c
        test_stb_sprintf.cpp
        test_support.cpp
)
target_include_directories(dicomweb_probe PRIVATE
        ../src
        ../src/dicom
        ../src/third_party
        ../src/utils
)
target_link_libraries(dicomweb_probe PRIVATE slidescape_nongui slidescape_jpeg)
slidescape_enable_process_allocator(dicomweb_probe)

# No external services, OpenSSL executable, or private fixtures are needed.
add_executable(slidescape_tls_tests test_tls.c)
set_target_properties(slidescape_tls_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(slidescape_tls_tests PRIVATE
        SLIDESCAPE_TLS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tls")
target_link_libraries(slidescape_tls_tests PRIVATE slidescape_tls)
slidescape_enable_process_allocator(slidescape_tls_tests)
add_test(NAME slidescape_tls_tests COMMAND slidescape_tls_tests)
set_tests_properties(slidescape_tls_tests PROPERTIES TIMEOUT 60 LABELS "tls")

# TODO: remove allocator benchmarks again once integration is stable?
if(BUILD_SLIDESCAPE_BENCHMARKS AND (WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        add_executable(slidescape_allocator_benchmark
                allocator_benchmark.cpp
                ../src/core/tile_buffer_pool.c
                ../src/platform/platform_mutex.c)
        target_include_directories(slidescape_allocator_benchmark PRIVATE
                ../src
                ../src/core
                ../src/platform
                ../src/utils
                ../src/third_party)
        target_compile_definitions(slidescape_allocator_benchmark PRIVATE USE_CONSOLE_LOGGING=1)
        slidescape_enable_process_allocator(slidescape_allocator_benchmark)
endif()
