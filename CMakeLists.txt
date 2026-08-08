cmake_minimum_required(VERSION 3.10)
project(slidescape)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
message(STATUS "CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")

option(BUILD_SLIDESCAPE_ARCHIVE "Build the pathology-first archive service" OFF)
option(USE_JPEG_SIMD "Use SIMD code for libjpeg-turbo" ON)
option(USE_ADDRESS_SANITIZER "Use address sanitizer" OFF)

if (APPLE)
    # For macOS: to produce a notarization-ready app bundle, sign with a Developer ID Application certificate, passing cmake flags:
    #   -DWITH_CODESIGN=ON -DMACOS_CODESIGN_IDENTITY="Developer ID Application: Your Team"
    option(WITH_CODESIGN "Code-sign the macOS app bundle after building" OFF)
    option(MACOS_BUNDLE_OPENSLIDE "Bundle OpenSlide and its non-system dylib dependencies in the macOS app bundle" ON)
    set(MACOS_CODESIGN_IDENTITY "Developer ID Application" CACHE STRING "Code-signing identity for the macOS app bundle")
    set(MACOS_CODESIGN_ENTITLEMENTS "" CACHE FILEPATH "Optional entitlements plist for signing the macOS app bundle")
    # NOTE: the script will by default look in `~/Downloads` for an OpenSlide binary distribution named `openslide-bin-*-macos-arm64-x86_64`
    set(MACOS_OPENSLIDE_DYLIB "" CACHE FILEPATH "Optional path to libopenslide.dylib for the macOS app bundle")
    set(MACOS_OPENSLIDE_ROOT "" CACHE PATH "Optional path to an official OpenSlide macOS binary distribution")
    option(SLIDESCAPE_ARCHIVE_USE_COMMONCRYPTO
            "Use the macOS CommonCrypto SHA-256 backend for the pathology archive"
            ON)
endif()

# Detect CPU type and whether we're building 64-bit or 32-bit code
math(EXPR BITS "${CMAKE_SIZEOF_VOID_P} * 8")
string(TOLOWER ${CMAKE_SYSTEM_PROCESSOR} CMAKE_SYSTEM_PROCESSOR_LC)
set(COUNT 1)
foreach(ARCH ${CMAKE_OSX_ARCHITECTURES})
    if(COUNT GREATER 1)
        message(FATAL_ERROR "${CMAKE_PROJECT_NAME} contains assembly code, so it cannot be built with multiple values in CMAKE_OSX_ARCHITECTURES.")
    endif()
    math(EXPR COUNT "${COUNT}+1")
endforeach()
if(CMAKE_SYSTEM_PROCESSOR_LC MATCHES "x86_64" OR
        CMAKE_SYSTEM_PROCESSOR_LC MATCHES "amd64" OR
        CMAKE_SYSTEM_PROCESSOR_LC MATCHES "i[0-9]86" OR
        CMAKE_SYSTEM_PROCESSOR_LC MATCHES "x86" OR
        CMAKE_SYSTEM_PROCESSOR_LC MATCHES "ia32")
    if(BITS EQUAL 64 OR CMAKE_C_COMPILER_ABI MATCHES "ELF X32")
        set(CPU_TYPE x86_64)
    else()
        set(CPU_TYPE i386)
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL ${CPU_TYPE})
        set(CMAKE_SYSTEM_PROCESSOR ${CPU_TYPE})
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR_LC STREQUAL "aarch64" OR
        CMAKE_SYSTEM_PROCESSOR_LC MATCHES "^arm")
    if(BITS EQUAL 64)
        set(CPU_TYPE arm64)
    else()
        set(CPU_TYPE arm)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR_LC MATCHES "^ppc" OR
        CMAKE_SYSTEM_PROCESSOR_LC MATCHES "^powerpc")
    set(CPU_TYPE powerpc)
else()
    set(CPU_TYPE ${CMAKE_SYSTEM_PROCESSOR_LC})
endif()
if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" OR
        CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR
        CMAKE_OSX_ARCHITECTURES MATCHES "i386")
    set(CPU_TYPE ${CMAKE_OSX_ARCHITECTURES})
endif()
if(CMAKE_OSX_ARCHITECTURES MATCHES "ppc")
    set(CPU_TYPE powerpc)
endif()
if(MSVC_IDE AND CMAKE_GENERATOR_PLATFORM MATCHES "arm64")
    set(CPU_TYPE arm64)
endif()
message(STATUS "${BITS}-bit build (${CPU_TYPE})")

# Set up compiler and linker flags
if (MSVC)
    set(COMMON_COMPILER_FLAGS "${MSVC_CPU_OPTIONS} ${MSVC_WARN_OPTIONS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_COMPILER_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_COMPILER_FLAGS}")
else()
    set(GCC_WARN_OPTIONS "-Wall -Wno-unused-function -Wno-unused-but-set-variable -Wno-unused-variable -Wno-missing-braces -Wno-format -Wno-strict-aliasing -Wno-shift-negative-value -Wno-sign-compare")
    if (APPLE)
        set(GCC_WARN_OPTIONS "${GCC_WARN_OPTIONS} -Wno-deprecated-declarations -Wno-unknown-warning-option")
    endif()

    if (CPU_TYPE STREQUAL "x86_64" OR CPU_TYPE STREQUAL "i386")
        set(GCC_CPU_OPTIONS "-mmmx -msse -msse2 -msse3 -mssse3 -msse4 -msse4.1 -msse4.2 -mavx") # Sandy Bridge CPU or later
    elseif(CPU_TYPE STREQUAL "arm64" OR CPU_TYPE STREQUAL "arm")
        if (APPLE)
            set(GCC_CPU_OPTIONS "-mcpu=native")
        else()
            set(GCC_CPU_OPTIONS "-march=native -mcpu=native")
        endif()
    endif()
    set(COMMON_COMPILER_FLAGS "${GCC_CPU_OPTIONS} ${GCC_WARN_OPTIONS} -fvisibility=hidden")
    if (USE_ADDRESS_SANITIZER)
        set(COMMON_COMPILER_FLAGS "${COMMON_COMPILER_FLAGS} -fsanitize=address")
    endif()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_COMPILER_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_COMPILER_FLAGS} -Wno-class-memaccess -Wno-conversion-null -Wno-deprecated-enum-enum-conversion")

    if (NOT(APPLE))
        # In MinSizeRel builds, further reduce file size by removing symbol table and relocation information from the executable
        set(CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL} -s")
        set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} -s")

        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++ -no-pie")
    endif()
    if (WIN32)
        # Static linkage on Windows to prevent depending on libwinpthread-1.dll
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    endif()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")

include_directories("${CMAKE_SOURCE_DIR}/src")
include_directories("${CMAKE_SOURCE_DIR}/src/core")
include_directories("${CMAKE_SOURCE_DIR}/src/dicom")
include_directories("${CMAKE_SOURCE_DIR}/src/imgui")
include_directories("${CMAKE_SOURCE_DIR}/src/isyntax")
include_directories("${CMAKE_SOURCE_DIR}/src/mrxs")
include_directories("${CMAKE_SOURCE_DIR}/src/platform")
include_directories("${CMAKE_SOURCE_DIR}/src/tiff")
include_directories("${CMAKE_SOURCE_DIR}/src/utils")
include_directories("${CMAKE_SOURCE_DIR}/src/third_party")
include_directories("${CMAKE_SOURCE_DIR}/deps")
include_directories("${CMAKE_SOURCE_DIR}/deps/jpeg")
include_directories("${CMAKE_SOURCE_DIR}/deps/keycode")
if (WIN32)
    include_directories("${CMAKE_SOURCE_DIR}/deps/dirent")
endif()

set(JPEG_SOURCE_FILES jcapimin.c
        wrapper/jcapistd-8.c wrapper/jcapistd-12.c wrapper/jcapistd-16.c
        wrapper/jccoefct-8.c wrapper/jccoefct-12.c
        wrapper/jccolor-8.c wrapper/jccolor-12.c wrapper/jccolor-16.c
        wrapper/jcdctmgr-8.c wrapper/jcdctmgr-12.c
        wrapper/jcdiffct-8.c wrapper/jcdiffct-12.c wrapper/jcdiffct-16.c
        jchuff.c jcicc.c jcinit.c jclhuff.c
        wrapper/jclossls-8.c wrapper/jclossls-12.c wrapper/jclossls-16.c
        wrapper/jcmainct-8.c wrapper/jcmainct-12.c wrapper/jcmainct-16.c
        jcmarker.c jcmaster.c jcomapi.c jcparam.c jcphuff.c
        wrapper/jcprepct-8.c wrapper/jcprepct-12.c wrapper/jcprepct-16.c
        wrapper/jcsample-8.c wrapper/jcsample-12.c wrapper/jcsample-16.c
        jctrans.c jdapimin.c
        wrapper/jdapistd-8.c wrapper/jdapistd-12.c wrapper/jdapistd-16.c
        jdatadst.c jdatasrc.c
        wrapper/jdcoefct-8.c wrapper/jdcoefct-12.c
        wrapper/jdcolor-8.c wrapper/jdcolor-12.c wrapper/jdcolor-16.c
        wrapper/jddctmgr-8.c wrapper/jddctmgr-12.c
        wrapper/jddiffct-8.c wrapper/jddiffct-12.c wrapper/jddiffct-16.c
        jdhuff.c jdicc.c jdinput.c jdlhuff.c
        wrapper/jdlossls-8.c wrapper/jdlossls-12.c wrapper/jdlossls-16.c
        wrapper/jdmainct-8.c wrapper/jdmainct-12.c wrapper/jdmainct-16.c
        jdmarker.c jdmaster.c
        wrapper/jdmerge-8.c wrapper/jdmerge-12.c
        jdphuff.c
        wrapper/jdpostct-8.c wrapper/jdpostct-12.c wrapper/jdpostct-16.c
        wrapper/jdsample-8.c wrapper/jdsample-12.c wrapper/jdsample-16.c
        jdtrans.c jerror.c jfdctflt.c
        wrapper/jfdctfst-8.c wrapper/jfdctfst-12.c
        wrapper/jfdctint-8.c wrapper/jfdctint-12.c
        wrapper/jidctflt-8.c wrapper/jidctflt-12.c
        wrapper/jidctfst-8.c wrapper/jidctfst-12.c
        wrapper/jidctint-8.c wrapper/jidctint-12.c
        wrapper/jidctred-8.c wrapper/jidctred-12.c
        jmemmgr.c jmemnobs.c jpeg_nbits.c
        wrapper/jquant1-8.c wrapper/jquant1-12.c
        wrapper/jquant2-8.c wrapper/jquant2-12.c
        wrapper/jutils-8.c wrapper/jutils-12.c wrapper/jutils-16.c
        jaricom.c
        jcarith.c
        jdarith.c
)

## libjpeg-turbo requires these files for JPEG decoding support
#set(JPEG_SOURCE_FILES
#        jerror.c    jaricom.c   jcapimin.c  jcmarker.c  jcomapi.c
#        jdapimin.c  jdapistd.c  jdapistd.c  jdarith.c   jdcoefct.c
#        jdcolor.c   jddctmgr.c  jdhuff.c    jdinput.c   jdmainct.c
#        jdmarker.c  jdmaster.c  jdmerge.c   jdphuff.c   jdpostct.c
#        jdsample.c  jidctflt.c  jidctfst.c  jidctint.c  jidctred.c
#        jmemmgr.c   jmemnobs.c  jquant1.c   jquant2.c   jutils.c
#)
#
## extra files needed for JPEG compression support
#set(JPEG_SOURCE_FILES ${JPEG_SOURCE_FILES}
#        jcapistd.c  jdatadst.c  jcparam.c   jcinit.c    jcmaster.c
#        jccolor.c   jcsample.c  jcprepct.c  jcdctmgr.c  jcarith.c
#        jctrans.c   jchuff.c    jccoefct.c  jcmainct.c  jcphuff.c
#        jfdctflt.c  jfdctfst.c  jfdctint.c
#)

# https://stackoverflow.com/questions/13103018/how-to-prepend-cmake-current-source-dir-to-all-files-in-variable
function(prepend_path SOURCE_FILES PREFIX)
    foreach(SOURCE_FILE ${${SOURCE_FILES}})
        set(MODIFIED ${MODIFIED} ${PREFIX}/${SOURCE_FILE})
    endforeach()
    set(${SOURCE_FILES} ${MODIFIED} PARENT_SCOPE)
endfunction()
prepend_path(JPEG_SOURCE_FILES deps/jpeg)

#set(WITH_SIMD 1) # Set to 0 to disable the SIMD code for libjpeg-turbo
if (USE_JPEG_SIMD)
    # NOTE: there is a bug with libjpeg-turbo SIMD on macOS/aarch64,
    # causing written JPEG tiles to be corrupted and/or have wrong colors.
    # To be sure, disable on arm64 platforms until this is fixed.
    if (NOT CPU_TYPE STREQUAL "arm64")
        set(WITH_SIMD 1)
    else()
        message("Disabling JPEG SIMD on arm64 to prevent a bug causing corrupted image writes")
    endif()
endif()
if (WITH_SIMD EQUAL 1)
    # requirements for deps/jpeg/simd/CMakeLists.txt
    include(CheckCSourceCompiles) # for check_c_source_compiles()
    macro(boolean_number var)
        if(${var})
            set(${var} 1)
        else()
            set(${var} 0)
        endif()
    endmacro()
    add_subdirectory(deps/jpeg/simd) # If this fails, WITH_SIMD will be reset to 0
endif()
if (WITH_SIMD EQUAL 1)
    set(JPEG_SOURCE_FILES ${JPEG_SOURCE_FILES} $<TARGET_OBJECTS:simd> ${SIMD_OBJS})
endif()
add_library(slidescape_jpeg STATIC ${JPEG_SOURCE_FILES})
if (WITH_SIMD EQUAL 1)
    target_compile_definitions(slidescape_jpeg PRIVATE SLIDESCAPE_JPEG_WITH_SIMD=1)
    target_compile_definitions(simd PRIVATE SLIDESCAPE_JPEG_WITH_SIMD=1)
endif()

set(OPENJPEG_SOURCE_FILES
        thread.c bio.c cio.c dwt.c event.c ht_dec.c image.c invert.c j2k.c jp2.c
        mct.c mqc.c openjpeg.c opj_clock.c pi.c t1.c t2.c tcd.c tgt.c
        function_list.c opj_malloc.c sparse_array.c)
prepend_path(OPENJPEG_SOURCE_FILES deps/openjpeg)
add_library(slidescape_openjpeg STATIC ${OPENJPEG_SOURCE_FILES})
target_include_directories(slidescape_openjpeg PUBLIC "${CMAKE_SOURCE_DIR}/deps/openjpeg")
target_compile_definitions(slidescape_openjpeg PUBLIC OPJ_STATIC=1)
if(UNIX)
    target_link_libraries(slidescape_openjpeg PUBLIC m)
endif()

set(FREETYPE_SOURCE_FILES
        autofit/autofit.c   base/ftbbox.c       base/ftbase.c       base/ftbdf.c        base/ftbitmap.c
        base/ftcid.c        base/ftfstype.c     base/ftgasp.c       base/ftglyph.c      base/ftgxval.c
        base/ftinit.c       base/ftmm.c         base/ftotval.c      base/ftpatent.c     base/ftpfr.c
        base/ftstroke.c     base/ftsynth.c      base/fttype1.c      base/ftwinfnt.c     bdf/bdf.c
        cache/ftcache.c     cff/cff.c           cid/type1cid.c      pcf/pcf.c           pfr/pfr.c
        psaux/psaux.c       pshinter/pshinter.c psnames/psnames.c   raster/raster.c     sfnt/sfnt.c
        smooth/smooth.c     truetype/truetype.c type1/type1.c       type42/type42.c     winfonts/winfnt.c
        base/ftsystem.c
)
prepend_path(FREETYPE_SOURCE_FILES deps/freetype/src)
if (WIN32)
    set(FREETYPE_SOURCE_FILES ${FREETYPE_SOURCE_FILES} deps/freetype/builds/windows/ftdebug.c)
else()
    set(FREETYPE_SOURCE_FILES ${FREETYPE_SOURCE_FILES} deps/freetype/src/base/ftdebug.c)
endif()

# mbedtls
set(MBEDTLS_CRYPTO_SOURCE_FILES
        aes.c                   aesni.c                         arc4.c                  aria.c              asn1parse.c
        asn1write.c             base64.c                        bignum.c                blowfish.c          camellia.c
        ccm.c                   chacha20.c                      chachapoly.c            cipher.c            cipher_wrap.c
        cmac.c                  ctr_drbg.c                      des.c                   dhm.c               ecdh.c
        ecdsa.c                 ecjpake.c                       ecp.c                   ecp_curves.c        entropy.c
        entropy_poll.c          error.c                         gcm.c                   havege.c            hkdf.c
        hmac_drbg.c             md.c                            md2.c                   md4.c               md5.c
        memory_buffer_alloc.c   nist_kw.c                       oid.c                   padlock.c           pem.c
        pk.c                    pk_wrap.c                       pkcs12.c                pkcs5.c             pkparse.c
        pkwrite.c               platform.c                      platform_util.c         poly1305.c          psa_crypto.c
        psa_crypto_se.c         psa_crypto_slot_management.c    psa_crypto_storage.c    psa_its_file.c      ripemd160.c
        rsa.c                   rsa_internal.c                  sha1.c                  sha256.c            sha512.c
        threading.c             timing.c                        version.c               version_features.c  xtea.c
        psa_crypto_client.c     psa_crypto_driver_wrappers.c    psa_crypto_ecp.c        psa_crypto_rsa.c
)
set(MBEDTLS_X509_SOURCE_FILES
        certs.c                 pkcs11.c                        x509.c                  x509_create.c       x509_crl.c
        x509_crt.c              x509_csr.c                      x509write_crt.c         x509write_csr.c
)
set(MBEDTLS_TLS_SOURCE_FILES
        debug.c                 net_sockets.c                   ssl_cache.c             ssl_ciphersuites.c  ssl_cli.c
        ssl_cookie.c            ssl_msg.c                       ssl_srv.c               ssl_ticket.c        ssl_tls.c
)
set(MBEDTLS_SOURCE_FILES ${MBEDTLS_CRYPTO_SOURCE_FILES} ${MBEDTLS_X509_SOURCE_FILES} ${MBEDTLS_TLS_SOURCE_FILES})
prepend_path(MBEDTLS_SOURCE_FILES deps/mbedtls/library)

if (WIN32)
    set(KEYCODE_SOURCE_FILES keycode_id.c windows_fromhid.c windows_lparam.c windows_name.c windows_rawname.c windows_tohid.c)
elseif(APPLE)
    set(KEYCODE_SOURCE_FILES keycode_id.c macos_fromhid.c macos_modifier.c macos_name.c macos_rawname.c macos_tohid.c)
else()
    set(KEYCODE_SOURCE_FILES keycode_id.c linux_fromhid.c linux_name.c linux_rawname.c linux_tohid.c)
endif()
prepend_path(KEYCODE_SOURCE_FILES deps/keycode)




set(SLIDESCAPE_NONGUI_BASE_FILES
        src/platform/platform.c
        src/platform/work_queue.c
        src/platform/openslide_api.c
        src/utils/stringutils.c
        src/utils/mathutils.c
        src/utils/memrw.c
        src/utils/jpeg_decoder.c
        src/utils/jpeg2000_decoder.c
        src/utils/crc32.c
        src/utils/block_allocator.c
        src/utils/timerutils.c
        src/platform/platform_mutex.c
        src/core/image.c
        src/core/image_resize.c
        src/core/image_loader.c
        src/core/tile_cache.c
        src/core/tile_loader.c
        src/core/http_request.c
        src/core/http_response_parser.c
        src/core/ssi_protocol.c
        src/core/ssi_transport.c
        src/utils/phasecorrelate.c
        src/third_party/lz4.c
        src/third_party/yxml.c
        src/third_party/minfft.c
        src/third_party/miniz_tinfl.c
        src/third_party/http_parser.c
        src/third_party/ltalloc.cc
        src/tiff/tiff.c
        src/tiff/tiff_write.c
        src/tiff/tiff_xml.c
        src/tiff/tif_lzw.c
        src/dicom/dicom.c
        src/dicom/dicom_dict.c
        src/dicom/dicom_uid.c
        src/dicom/dicomweb.c
        src/dicom/dicom_wsi.c
        src/isyntax/isyntax.c
        src/isyntax/isyntax_reader.c
        src/isyntax/isyntax_streamer.c
        src/isyntax/libisyntax.c
        src/mrxs/mrxs.c
)
if (WIN32)
    list(APPEND SLIDESCAPE_NONGUI_BASE_FILES src/platform/win32_utils.c)
else()
    list(APPEND SLIDESCAPE_NONGUI_BASE_FILES src/platform/linux_utils.c)
endif()
add_library(slidescape_nongui STATIC ${SLIDESCAPE_NONGUI_BASE_FILES})
target_compile_definitions(slidescape_nongui PRIVATE
        IS_SERVER=0
        ISYNTAX_JPEG_DECODER_USE_LIBJPEG=1
)
target_link_libraries(slidescape_nongui PUBLIC slidescape_openjpeg)




set(VIEWER_SOURCE_FILES
        platform/graphical_app.c
        platform/presenter.cpp
        platform/presenter_imgui_opengl_cached.cpp
        core/viewer.cpp
        core/viewer_io_file.cpp
        core/viewer_io_remote.c
        core/viewer_options.c
        core/renderer_opengl.c
        core/renderer_opengl_shader.c
        core/app_input.cpp
        core/commandline.cpp
        core/scene.cpp
        core/gui.cpp
        core/console.cpp
        core/annotation.cpp
        core/annotation_asap_xml.cpp
        core/annotation_geojson.cpp
        core/ini.c
        core/remote.c
        core/dicomweb_client.c
        core/slide_score.c
        core/ssi_client.c
        core/image_registration.c
        imgui/imgui.cpp
        imgui/imgui_demo.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_widgets.cpp
        imgui/imgui_tables.cpp
        imgui/backends/imgui_impl_opengl3.cpp
        imgui/misc/freetype/imgui_freetype.cpp
        imgui/imgui_widget_flamegraph.cpp
        utils/triangulate.c
        utils/profiler.c
        )
if (WIN32)
    set(VIEWER_SOURCE_FILES ${VIEWER_SOURCE_FILES}
            platform/win32_main.cpp
            platform/presenter_backend_win32_opengl.cpp
            platform/win32_utils.c
            platform/win32_gui.cpp
            platform/glad.c
            imgui/backends/imgui_impl_win32.cpp
            )
else()
    # Note: SDL2 is used for both Linux and macOS
    set(VIEWER_SOURCE_FILES ${VIEWER_SOURCE_FILES}
            platform/linux_main.cpp
            platform/presenter_backend_sdl_opengl.cpp
            platform/linux_utils.c
            platform/linux_gui.cpp
            imgui/backends/imgui_impl_sdl2.cpp
            imgui/ImGuiFileDialog.cpp
            )
endif()
if (APPLE)
    set(VIEWER_SOURCE_FILES ${VIEWER_SOURCE_FILES}
            platform/macos_url_handler.mm
            )
endif()
prepend_path(VIEWER_SOURCE_FILES src)

set(LIBRARY_SOURCE_FILES
        ${FREETYPE_SOURCE_FILES}
        ${MBEDTLS_SOURCE_FILES}
        ${KEYCODE_SOURCE_FILES})





# client only supported on Windows x64 and macOS x64 for now
if (WIN32)
    add_executable(slidescape         ${VIEWER_SOURCE_FILES} ${LIBRARY_SOURCE_FILES} resources/icon/icon.rc)
    add_executable(slidescape_console ${VIEWER_SOURCE_FILES} ${LIBRARY_SOURCE_FILES} resources/icon/icon.rc)

    target_link_libraries(slidescape         slidescape_nongui slidescape_jpeg user32 gdi32 winmm imm32 dwmapi ws2_32 psapi shlwapi crypt32)
    target_link_libraries(slidescape_console slidescape_nongui slidescape_jpeg user32 gdi32 winmm imm32 dwmapi ws2_32 psapi shlwapi crypt32)

    # Hide the console window on (non-console) release builds
    target_link_options(slidescape PRIVATE "$<$<NOT:$<CONFIG:Debug>>:-mwindows>")
    target_compile_definitions(slidescape_console PRIVATE DONT_REGISTER_FILETYPE_ASSOCIATIONS=1) # associate only non-console buils
elseif(APPLE)
    set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} -headerpad_max_install_names")
    find_package(SDL2 REQUIRED)
    include_directories(${SDL2_INCLUDE_DIRS})
    set(MACOSX_BUNDLE_ICON_FILE "resources/icon/icon.icns")
    set_source_files_properties(${MACOSX_BUNDLE_ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    add_executable(slidescape MACOSX_BUNDLE ${VIEWER_SOURCE_FILES} ${LIBRARY_SOURCE_FILES} ${MACOSX_BUNDLE_ICON_FILE})
    set_target_properties(slidescape PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/Info.plist")
    target_link_libraries(slidescape slidescape_nongui slidescape_jpeg "-framework Cocoa" "-framework Quartz" "-framework OpenGL" "-framework ApplicationServices" "-framework Security")
    target_link_libraries(slidescape ${SDL2_LIBRARIES})
    set_target_properties(slidescape PROPERTIES OUTPUT_NAME Slidescape)

    # Copy bundled dylibs into the app and rewrite install names so the app can
    # run on machines without Homebrew/MacPorts.
    set(BUNDLE_BINARY_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Slidescape.app/Contents/MacOS")
    set(ARCHITECTURE "${CPU_TYPE}")
    execute_process(COMMAND sdl2-config --prefix OUTPUT_VARIABLE SDL2_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(SDL2_DYLIB "${SDL2_PREFIX}/lib/libSDL2.dylib")
    if(MACOS_BUNDLE_OPENSLIDE AND NOT MACOS_OPENSLIDE_DYLIB AND MACOS_OPENSLIDE_ROOT)
        set(OPENSLIDE_CANDIDATE "${MACOS_OPENSLIDE_ROOT}/lib/libopenslide.dylib")
        if(EXISTS "${OPENSLIDE_CANDIDATE}")
            set(MACOS_OPENSLIDE_DYLIB "${OPENSLIDE_CANDIDATE}")
        else()
            message(WARNING "MACOS_OPENSLIDE_ROOT does not contain lib/libopenslide.dylib: ${MACOS_OPENSLIDE_ROOT}")
        endif()
    endif()
    if(MACOS_BUNDLE_OPENSLIDE AND NOT MACOS_OPENSLIDE_DYLIB)
        file(GLOB OPENSLIDE_OFFICIAL_DISTS
                "$ENV{HOME}/Downloads/openslide-bin-*-macos-arm64-x86_64"
                "$ENV{HOME}/Downloads/openslide-bin-*-macos-x86_64-arm64")
        list(SORT OPENSLIDE_OFFICIAL_DISTS)
        list(REVERSE OPENSLIDE_OFFICIAL_DISTS)
        foreach(OPENSLIDE_DIST ${OPENSLIDE_OFFICIAL_DISTS})
            set(OPENSLIDE_CANDIDATE "${OPENSLIDE_DIST}/lib/libopenslide.dylib")
            if(EXISTS "${OPENSLIDE_CANDIDATE}")
                set(MACOS_OPENSLIDE_DYLIB "${OPENSLIDE_CANDIDATE}")
                break()
            endif()
        endforeach()
    endif()
    if(MACOS_BUNDLE_OPENSLIDE AND MACOS_OPENSLIDE_DYLIB)
        message(STATUS "Bundling OpenSlide from ${MACOS_OPENSLIDE_DYLIB}")
    elseif(MACOS_BUNDLE_OPENSLIDE)
        message(WARNING "MACOS_BUNDLE_OPENSLIDE is ON, but no official OpenSlide macOS binary distribution was found")
    endif()

    add_custom_command(TARGET slidescape POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DBUNDLE_BINARY_DIR="${BUNDLE_BINARY_DIR}"
                -DARCHITECTURE="${ARCHITECTURE}"
                -DSDL2_DYLIB="${SDL2_DYLIB}"
                -DOPENSLIDE_DYLIB="${MACOS_OPENSLIDE_DYLIB}"
                -P "${CMAKE_SOURCE_DIR}/resources/bundle_macos_dylibs.cmake"
            COMMAND cp -f "${CMAKE_SOURCE_DIR}/src/Info.plist" "${BUNDLE_BINARY_DIR}/../Info.plist")
    if (WITH_CODESIGN)
        if(MACOS_CODESIGN_ENTITLEMENTS)
            set(MACOS_CODESIGN_ENTITLEMENTS_ARGS --entitlements "${MACOS_CODESIGN_ENTITLEMENTS}")
        else()
            set(MACOS_CODESIGN_ENTITLEMENTS_ARGS "")
        endif()
        add_custom_command(TARGET slidescape POST_BUILD
                COMMAND codesign --force --deep --timestamp --options runtime
                    --sign "${MACOS_CODESIGN_IDENTITY}"
                    ${MACOS_CODESIGN_ENTITLEMENTS_ARGS}
                    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Slidescape.app")
    endif()
else()
    add_executable(slidescape ${VIEWER_SOURCE_FILES} ${LIBRARY_SOURCE_FILES})
    find_package(SDL2 REQUIRED)
    include_directories(${SDL2_INCLUDE_DIRS})
    target_link_libraries(slidescape slidescape_nongui slidescape_jpeg pthread dl m GL GLEW SDL2)

    # Create slidescape.desktop and install/uninstall scripts, and copy them to the output directory
    configure_file(resources/slidescape.desktop.in slidescape.desktop)
    add_custom_command(TARGET slidescape POST_BUILD
            COMMAND cp -f slidescape.desktop "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/slidescape.desktop"
            COMMAND cp -f "${CMAKE_SOURCE_DIR}/resources/install_desktop_file.sh.in" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/install_desktop_file.sh"
            COMMAND cp -f "${CMAKE_SOURCE_DIR}/resources/uninstall_desktop_file.sh.in" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/uninstall_desktop_file.sh"
            COMMAND chmod +x "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/slidescape.desktop"
            COMMAND chmod +x "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/install_desktop_file.sh"
            COMMAND chmod +x "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/uninstall_desktop_file.sh")
endif()
target_compile_definitions(slidescape PRIVATE IS_SERVER=0)





# Separately compiled tools:
# slidescape_archive - standalone pathology-first archive service [early development]
# dicom_dict_gen - tool for generating a DICOM dictionary (dicom_dict.h and dicom_dict.c) by parsing the DICOM standard.

# Some files are always required for the separately compiled tools
set(BASE_FILES
        src/platform/platform.c
        )
if (WIN32)
    list(APPEND BASE_FILES src/platform/win32_utils.c)
else()
    list(APPEND BASE_FILES src/platform/linux_utils.c)
endif()

add_library(slidescape_sqlite STATIC src/third_party/sqlite3.c)
target_include_directories(slidescape_sqlite PUBLIC src/third_party)
target_compile_definitions(slidescape_sqlite PRIVATE
        SQLITE_DEFAULT_FOREIGN_KEYS=1
        SQLITE_DQS=0
        SQLITE_OMIT_LOAD_EXTENSION=1
        SQLITE_THREADSAFE=1
)

set(SLIDESCAPE_ARCHIVE_CORE_FILES
        src/archive/archive_database.c
        src/archive/archive_frame_map.c
        src/archive/archive_object.c
        src/archive/archive_repository.c
        src/archive/archive_routes.c
        src/archive/archive_sha256.c
        src/archive/archive_shell.c
        src/archive/archive_shell_recipe.c
        src/archive/archive_index.c
        src/archive/archive_ingest.c
        src/archive/archive_mrxs_adapter.c
        src/archive/archive_representation.c
        src/archive/archive_instance_provider.c
        src/dicom/dicom_uid.c
)
if (NOT(APPLE AND SLIDESCAPE_ARCHIVE_USE_COMMONCRYPTO))
    list(APPEND SLIDESCAPE_ARCHIVE_CORE_FILES src/third_party/sha256.c)
endif()
add_library(slidescape_archive_core STATIC ${SLIDESCAPE_ARCHIVE_CORE_FILES})
target_include_directories(slidescape_archive_core PUBLIC src/archive)
target_compile_definitions(slidescape_archive_core PRIVATE IS_SERVER=1)
target_link_libraries(slidescape_archive_core PRIVATE slidescape_sqlite)
if (APPLE AND SLIDESCAPE_ARCHIVE_USE_COMMONCRYPTO)
    find_library(SECURITY_FRAMEWORK Security)
    if (NOT SECURITY_FRAMEWORK)
        message(FATAL_ERROR "Security.framework is required for the CommonCrypto archive SHA-256 backend")
    endif()
    target_compile_definitions(slidescape_archive_core PUBLIC
            ARCHIVE_SHA256_USE_COMMON_CRYPTO=1)
    target_link_libraries(slidescape_archive_core PRIVATE ${SECURITY_FRAMEWORK})
else()
    target_compile_definitions(slidescape_archive_core PUBLIC
            ARCHIVE_SHA256_USE_COMMON_CRYPTO=0)
endif()

if (BUILD_SLIDESCAPE_ARCHIVE)
    add_executable(slidescape_archive
            src/archive/archive_main.c
            src/archive/archive_http.c
            src/archive/archive_support.c
            )
    target_compile_definitions(slidescape_archive PRIVATE IS_SERVER=1)
    target_link_libraries(slidescape_archive PRIVATE slidescape_archive_core slidescape_nongui slidescape_jpeg)
    if (WIN32)
        target_link_libraries(slidescape_archive PRIVATE ws2_32 winmm)
    endif()
endif()

add_executable(dicom_dict_gen
        ${BASE_FILES}
        src/dicom/dicom_dict_gen.c
        src/utils/memrw.c
        src/utils/stringutils.c
        src/third_party/yxml.c
        src/third_party/lz4.c
        src/third_party/ltalloc.cc
        )
target_compile_definitions(dicom_dict_gen PRIVATE IS_SERVER=1)

include(CTest)

if (BUILD_TESTING)
    add_subdirectory(tests)
endif()
