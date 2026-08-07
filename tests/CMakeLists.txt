
add_executable(slidescape_tests
        test_main.cpp
        test_fixtures.cpp
        test_dicomweb.cpp
        test_archive.cpp
        test_http_response_parser.cpp
        test_http_request.cpp
        test_mathutils.cpp
        test_memrw.cpp
        test_stb_sprintf.cpp
        test_slide_score.cpp
        test_ssi.cpp
        test_support.cpp
        test_stringutils.cpp
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
        IS_SERVER=0
        SLIDESCAPE_TEST_FIXTURE_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/manifest.txt"
        SLIDESCAPE_TEST_LOCAL_FIXTURE_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/local_manifest.txt"
        SLIDESCAPE_TEST_DICOMWEB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dicomweb"
        ISYNTAX_JPEG_DECODER_USE_LIBJPEG=1
)
target_link_libraries(slidescape_tests PRIVATE slidescape_nongui slidescape_jpeg slidescape_archive_core)
if (WIN32)
        target_link_libraries(slidescape_tests PRIVATE winmm)
endif()

add_test(NAME slidescape_tests COMMAND slidescape_tests)

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
