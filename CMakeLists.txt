cmake_minimum_required(VERSION 3.15)
project(slidescape)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
message(STATUS "CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")

option(BUILD_SLIDESCAPE_ARCHIVE "Build the pathology-first archive service" OFF)
option(BUILD_SLIDESCAPE_BENCHMARKS "Build standalone Slidescape microbenchmarks" OFF)
option(USE_JPEG_SIMD "Use SIMD code for libjpeg-turbo" ON)
option(USE_ADDRESS_SANITIZER "Use address sanitizer" OFF)

option(SLIDESCAPE_MSVC_STATIC_RUNTIME "Use the static MSVC runtime (/MT or /MTd)" OFF)
set(SLIDESCAPE_MIMALLOC_SUPPORTED OFF)
if((WIN32 AND MSVC) OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SLIDESCAPE_MIMALLOC_SUPPORTED ON)
endif()
set(SLIDESCAPE_MIMALLOC_DEFAULT ${SLIDESCAPE_MIMALLOC_SUPPORTED})
if(USE_ADDRESS_SANITIZER AND NOT MSVC)
    set(SLIDESCAPE_MIMALLOC_DEFAULT OFF)
endif()
option(SLIDESCAPE_USE_MIMALLOC "Use a static mimalloc malloc override" ${SLIDESCAPE_MIMALLOC_DEFAULT})
set(SLIDESCAPE_MIMALLOC_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/mimalloc" CACHE PATH "Path to a mimalloc source checkout")

if(SLIDESCAPE_USE_MIMALLOC AND NOT SLIDESCAPE_MIMALLOC_SUPPORTED)
    message(FATAL_ERROR "SLIDESCAPE_USE_MIMALLOC currently supports only Windows/MSVC, Linux, and macOS")
endif()
if(SLIDESCAPE_USE_MIMALLOC AND USE_ADDRESS_SANITIZER AND NOT MSVC)
    message(FATAL_ERROR "SLIDESCAPE_USE_MIMALLOC is incompatible with USE_ADDRESS_SANITIZER; disable mimalloc for sanitizer builds")
endif()
if(SLIDESCAPE_USE_MIMALLOC)
    if(MSVC)
        set(SLIDESCAPE_MSVC_STATIC_RUNTIME ON)
    endif()
endif()
if(MSVC AND SLIDESCAPE_MSVC_STATIC_RUNTIME)
    # Link the runtime library statically, with or without debug symbols depending on the configuration
    # see: https://cmake.org/cmake/help/latest/variable/CMAKE_MSVC_RUNTIME_LIBRARY.html#variable:CMAKE_MSVC_RUNTIME_LIBRARY
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    set(COMMON_COMPILER_FLAGS "${MSVC_CPU_OPTIONS} ${MSVC_WARN_OPTIONS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_COMPILER_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_COMPILER_FLAGS}")
else()
    set(GCC_WARN_OPTIONS "-Wall -Wformat=2 -Wno-unused-function -Wno-unused-but-set-variable -Wno-unused-variable -Wno-missing-braces -Wno-strict-aliasing -Wno-shift-negative-value -Wno-sign-compare")
    if (APPLE)
        set(GCC_WARN_OPTIONS "${GCC_WARN_OPTIONS} -Wno-deprecated-declarations -Wno-unknown-warning-option")
    endif()

    if (CPU_TYPE STREQUAL "x86_64" OR CPU_TYPE STREQUAL "i386")
        if (WIN32)
            # For now, disable AVX globally on Windows: MinGW can emit aligned AVX stack accesses in
            # functions whose incoming stack is only guaranteed to be 16-byte aligned.
            # TODO: isolate the compiler bug causing this; restore AVX support
            set(GCC_CPU_OPTIONS "-mmmx -msse -msse2 -msse3 -mssse3 -msse4 -msse4.1 -msse4.2")
        else()
            set(GCC_CPU_OPTIONS "-mmmx -msse -msse2 -msse3 -mssse3 -msse4 -msse4.1 -msse4.2 -mavx")
        endif()
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

if(SLIDESCAPE_USE_MIMALLOC)
    if(NOT EXISTS "${SLIDESCAPE_MIMALLOC_SOURCE_DIR}/CMakeLists.txt" OR
       NOT EXISTS "${SLIDESCAPE_MIMALLOC_SOURCE_DIR}/src/static.c")
        message(FATAL_ERROR
                "mimalloc source not found at '${SLIDESCAPE_MIMALLOC_SOURCE_DIR}'. "
                "Vendor mimalloc there or set SLIDESCAPE_MIMALLOC_SOURCE_DIR.")
    endif()
    set(MI_OVERRIDE ON CACHE BOOL "" FORCE)
    set(MI_WIN_REDIRECT OFF CACHE BOOL "" FORCE)
    if(APPLE)
        set(MI_OSX_INTERPOSE ON CACHE BOOL "" FORCE)
        set(MI_OSX_ZONE ON CACHE BOOL "" FORCE)
    endif()
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    # Upstream's mimalloc-obj packaging target currently depends on the static
    # target as well, even though Slidescape links the object target directly.
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT ON CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_SECURE OFF CACHE STRING "" FORCE)
    add_subdirectory("${SLIDESCAPE_MIMALLOC_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}/mimalloc" EXCLUDE_FROM_ALL)
    add_compile_definitions(SLIDESCAPE_USE_MIMALLOC=1)
endif()

function(slidescape_enable_process_allocator target)
    if(SLIDESCAPE_USE_MIMALLOC)
        target_link_libraries(${target} PRIVATE mimalloc-obj)
    endif()
endfunction()

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

# Mbed TLS 4.2.0 + TF-PSA-Crypto 1.2.0. See doc/tls.md for the vendoring recipe.


set(TF_PSA_CRYPTO_SOURCE_FILES
        core/psa_crypto.c
        core/psa_crypto_client.c
        core/psa_crypto_driver_wrappers_no_static.c
        core/psa_crypto_random.c
        core/psa_crypto_slot_management.c
        core/psa_crypto_storage.c
        core/psa_its_file.c
        core/psa_util.c
        core/tf_psa_crypto_config.c
        core/tf_psa_crypto_version.c
        drivers/builtin/src/aes.c
        drivers/builtin/src/aesce.c
        drivers/builtin/src/aesni.c
        drivers/builtin/src/aria.c
        drivers/builtin/src/bignum.c
        drivers/builtin/src/bignum_core.c
        drivers/builtin/src/bignum_mod.c
        drivers/builtin/src/bignum_mod_raw.c
        drivers/builtin/src/block_cipher.c
        drivers/builtin/src/camellia.c
        drivers/builtin/src/ccm.c
        drivers/builtin/src/chacha20.c
        drivers/builtin/src/chacha20_neon.c
        drivers/builtin/src/chachapoly.c
        drivers/builtin/src/cipher.c
        drivers/builtin/src/cipher_wrap.c
        drivers/builtin/src/cmac.c
        drivers/builtin/src/ctr_drbg.c
        drivers/builtin/src/ecdsa.c
        drivers/builtin/src/ecjpake.c
        drivers/builtin/src/ecp.c
        drivers/builtin/src/ecp_curves.c
        drivers/builtin/src/entropy.c
        drivers/builtin/src/entropy_poll.c
        drivers/builtin/src/gcm.c
        drivers/builtin/src/hmac_drbg.c
        drivers/builtin/src/md5.c
        drivers/builtin/src/poly1305.c
        drivers/builtin/src/psa_crypto_aead.c
        drivers/builtin/src/psa_crypto_cipher.c
        drivers/builtin/src/psa_crypto_ecp.c
        drivers/builtin/src/psa_crypto_ffdh.c
        drivers/builtin/src/psa_crypto_hash.c
        drivers/builtin/src/psa_crypto_mac.c
        drivers/builtin/src/psa_crypto_pake.c
        drivers/builtin/src/psa_crypto_rsa.c
        drivers/builtin/src/psa_crypto_xof.c
        drivers/builtin/src/psa_util_internal.c
        drivers/builtin/src/ripemd160.c
        drivers/builtin/src/rsa.c
        drivers/builtin/src/rsa_alt_helpers.c
        drivers/builtin/src/sha1.c
        drivers/builtin/src/sha256.c
        drivers/builtin/src/sha3.c
        drivers/builtin/src/sha512.c
        extras/lmots.c
        extras/lms.c
        extras/md.c
        extras/nist_kw.c
        extras/pk.c
        extras/pk_ecc.c
        extras/pk_rsa.c
        extras/pk_wrap.c
        extras/pkparse.c
        extras/pkwrite.c
        platform/memory_buffer_alloc.c
        platform/platform.c
        platform/platform_util.c
        platform/threading.c
        utilities/asn1parse.c
        utilities/asn1write.c
        utilities/base64.c
        utilities/constant_time.c
        utilities/oid.c
        utilities/pem.c
        utilities/pkcs5.c
)
prepend_path(TF_PSA_CRYPTO_SOURCE_FILES deps/tf-psa-crypto)

set(MBEDTLS_SOURCE_FILES
        debug.c
        error.c
        mbedtls_config.c
        mps_reader.c
        mps_trace.c
        net_sockets.c
        pkcs7.c
        ssl_cache.c
        ssl_ciphersuites.c
        ssl_client.c
        ssl_cookie.c
        ssl_debug_helpers_generated.c
        ssl_msg.c
        ssl_ticket.c
        ssl_tls.c
        ssl_tls12_client.c
        ssl_tls12_server.c
        ssl_tls13_client.c
        ssl_tls13_generic.c
        ssl_tls13_keys.c
        ssl_tls13_server.c
        timing.c
        version.c
        version_features.c
        x509.c
        x509_create.c
        x509_crl.c
        x509_crt.c
        x509_csr.c
        x509_oid.c
        x509write.c
        x509write_crt.c
        x509write_csr.c
)
prepend_path(MBEDTLS_SOURCE_FILES deps/mbedtls/library)

add_library(slidescape_crypto STATIC ${TF_PSA_CRYPTO_SOURCE_FILES} src/platform/tls_platform.c)
target_include_directories(slidescape_crypto PUBLIC
        deps/tf-psa-crypto/include
        deps/tf-psa-crypto/drivers/builtin/include
        src/platform) # for configuration headers
target_include_directories(slidescape_crypto PRIVATE
        deps/tf-psa-crypto/core
        deps/tf-psa-crypto/dispatch
        deps/tf-psa-crypto/extras
        deps/tf-psa-crypto/platform
        deps/tf-psa-crypto/utilities
        deps/tf-psa-crypto/drivers/builtin/src)
target_compile_definitions(slidescape_crypto PUBLIC
        TF_PSA_CRYPTO_CONFIG_FILE="slidescape_crypto_config.h")
if (WIN32)
    target_link_libraries(slidescape_crypto PUBLIC bcrypt)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(slidescape_crypto PUBLIC Threads::Threads)
endif()

add_library(slidescape_tls STATIC ${MBEDTLS_SOURCE_FILES})
target_include_directories(slidescape_tls PUBLIC deps/mbedtls/include)
target_include_directories(slidescape_tls PRIVATE
        deps/mbedtls/library
        deps/tf-psa-crypto/core
        deps/tf-psa-crypto/dispatch
        deps/tf-psa-crypto/extras
        deps/tf-psa-crypto/platform
        deps/tf-psa-crypto/utilities
        deps/tf-psa-crypto/drivers/builtin/src)
target_compile_definitions(slidescape_tls PUBLIC
        MBEDTLS_CONFIG_FILE="slidescape_tls_config.h")
target_link_libraries(slidescape_tls PUBLIC slidescape_crypto)
if (WIN32)
    target_link_libraries(slidescape_tls PUBLIC ws2_32)
endif()

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
        src/platform/source_io.c
        src/platform/source_zip.c
        src/platform/source_fs.c
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
        src/core/tile_buffer_pool.c
        src/core/tile_loader.c
        src/core/http_request.c
        src/core/http_response_parser.c
        src/core/remote_url.c
        src/core/ssi_protocol.c
        src/core/ssi_transport.c
        src/utils/phasecorrelate.c
        src/third_party/lz4.c
        src/third_party/yxml.c
        src/third_party/minfft.c
        src/third_party/miniz.c
        src/third_party/http_parser.c
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
# Shared code calls logging functions supplied by the linking application or tests.
target_compile_definitions(slidescape_nongui PRIVATE
        USE_CONSOLE_LOGGING=1
        ISYNTAX_JPEG_DECODER_USE_LIBJPEG=1
)
target_link_libraries(slidescape_nongui PUBLIC slidescape_openjpeg)




set(VIEWER_SOURCE_FILES
        platform/graphical_app.c
        platform/presenter.cpp
        platform/presenter_imgui_opengl_cached.cpp
        core/viewer.cpp
        core/viewer_io_file.cpp
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
        ${KEYCODE_SOURCE_FILES})





# client only supported on Windows x64 and macOS x64 for now
if (WIN32)
    add_executable(slidescape         ${VIEWER_SOURCE_FILES} ${LIBRARY_SOURCE_FILES} resources/icon/icon.rc)
    add_executable(slidescape_console ${VIEWER_SOURCE_FILES} ${LIBRARY_SOURCE_FILES} resources/icon/icon.rc)

    target_link_libraries(slidescape         PRIVATE user32 gdi32 winmm imm32 dwmapi ws2_32 psapi shlwapi crypt32)
    target_link_libraries(slidescape_console PRIVATE user32 gdi32 winmm imm32 dwmapi ws2_32 psapi shlwapi crypt32)
    slidescape_enable_process_allocator(slidescape_console)

    target_compile_definitions(slidescape_console PRIVATE DONT_REGISTER_FILETYPE_ASSOCIATIONS=1) # associate only non-console builds

    # Hide the console window on non-Debug GUI builds.
    if (MSVC)
        target_link_options(slidescape PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS>" "$<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>")
    else()
        target_link_options(slidescape PRIVATE "$<$<NOT:$<CONFIG:Debug>>:-mwindows>")
    endif()

    # Suppress MSVC creating a second default manifest with resource ID 1 (the manifest is already explicitly embedded in icon.rc)
    if (MSVC)
        target_link_options(slidescape PRIVATE /MANIFEST:NO)
        target_link_options(slidescape_console PRIVATE /MANIFEST:NO)
    endif()
elseif(APPLE)
    set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} -headerpad_max_install_names")
    find_package(SDL2 REQUIRED)
    include_directories(${SDL2_INCLUDE_DIRS})
    set(MACOSX_BUNDLE_ICON_FILE "resources/icon/icon.icns")
    set_source_files_properties(${MACOSX_BUNDLE_ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    add_executable(slidescape MACOSX_BUNDLE ${VIEWER_SOURCE_FILES} ${LIBRARY_SOURCE_FILES} ${MACOSX_BUNDLE_ICON_FILE})
    set_target_properties(slidescape PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/Info.plist")
    target_link_libraries(slidescape PRIVATE "-framework Cocoa" "-framework Quartz" "-framework OpenGL" "-framework ApplicationServices" "-framework Security")
    target_link_libraries(slidescape PRIVATE ${SDL2_LIBRARIES})
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
    target_link_libraries(slidescape PRIVATE pthread dl m GL GLEW SDL2)

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
slidescape_enable_process_allocator(slidescape)
target_link_libraries(slidescape PRIVATE slidescape_nongui slidescape_jpeg slidescape_tls)
if (WIN32)
    target_link_libraries(slidescape_console PRIVATE slidescape_nongui slidescape_jpeg slidescape_tls)
endif()
# Route viewer logs through console.cpp; tools use stdio by default.
target_compile_definitions(slidescape PRIVATE USE_CONSOLE_LOGGING=1)
if (WIN32)
    target_compile_definitions(slidescape_console PRIVATE USE_CONSOLE_LOGGING=1)
endif()





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
        src/archive/archive_zip.c
        src/archive/archive_dicom_zip.c
        src/archive/archive_object.c
        src/archive/archive_repository.c
        src/archive/archive_routes.c
        src/archive/archive_sha256.c
        src/archive/archive_shell.c
        src/archive/archive_shell_recipe.c
        src/archive/archive_index.c
        src/archive/archive_ingest.c
        src/archive/archive_metadata.c
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
    target_link_libraries(slidescape_archive PRIVATE slidescape_archive_core slidescape_nongui slidescape_jpeg)
    slidescape_enable_process_allocator(slidescape_archive)
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
        )
slidescape_enable_process_allocator(dicom_dict_gen)

include(CTest)

if (BUILD_TESTING)
    add_subdirectory(tests)
endif()
