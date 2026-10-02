# CMake toolchain file for building WoWPS as a native PlayStation 5 app.
#
# Usage:
#   export PS5_VULKAN=/path/to/PS5_Vulkan   (mihawk-99/PS5_Vulkan checkout with
#                                            .deps/native/ps5-payload-sdk and
#                                            .deps/native/radv-release built)
#   cmake -S . -B build-ps5 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/ps5.toolchain.cmake
#   cmake --build build-ps5
#   tools/ps5/link.sh build-ps5
#
# The compiler target and flags are PS5_Vulkan's tooling/prospero-clang18: host
# clang targeting x86_64-sie-ps5 against the pinned payload SDK fork. CMake only
# compiles; tools/ps5/link.sh does the final link with RADV, the SDK platform
# layer and the native-app converter, the same recipe the RADV titles use.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CROSSCOMPILING TRUE)
set(WOWEE_PS5 ON CACHE BOOL "Building for PlayStation 5 (payload SDK + RADV)" FORCE)

if(NOT DEFINED PS5_VULKAN)
    if(DEFINED ENV{PS5_VULKAN})
        set(PS5_VULKAN "$ENV{PS5_VULKAN}")
    else()
        get_filename_component(PS5_VULKAN "${CMAKE_CURRENT_LIST_DIR}/../../deps/PS5_Vulkan" ABSOLUTE)
    endif()
endif()
set(PS5_VULKAN "${PS5_VULKAN}" CACHE PATH "PS5_Vulkan checkout (SDK fork + RADV)")
set(PS5_SDK "${PS5_VULKAN}/.deps/native/ps5-payload-sdk" CACHE PATH "Pinned PS5 payload SDK")
set(PS5_RADV "${PS5_VULKAN}/.deps/native/radv-release" CACHE PATH "RADV release build")

if(NOT EXISTS "${PS5_SDK}/bin/prospero-lld" OR NOT EXISTS "${PS5_SDK}/target/include")
    message(FATAL_ERROR "PS5_SDK=${PS5_SDK} is not a built payload SDK; run PS5_Vulkan's tools/setup-native-dependencies.sh")
endif()
if(NOT EXISTS "${PS5_RADV}/lib/libvulkan_radeon.ps5.a")
    message(FATAL_ERROR "PS5_RADV=${PS5_RADV} has no RADV archive; run PS5_Vulkan's tools/build-radv.sh release")
endif()

find_program(WOWEE_PS5_CLANG   NAMES clang REQUIRED)
find_program(WOWEE_PS5_CLANGXX NAMES clang++ REQUIRED)
find_program(WOWEE_PS5_AR      NAMES llvm-ar ar REQUIRED)
find_program(WOWEE_PS5_RANLIB  NAMES llvm-ranlib ranlib REQUIRED)

set(CMAKE_C_COMPILER   "${WOWEE_PS5_CLANG}")
set(CMAKE_CXX_COMPILER "${WOWEE_PS5_CLANGXX}")
set(CMAKE_AR           "${WOWEE_PS5_AR}")
set(CMAKE_RANLIB       "${WOWEE_PS5_RANLIB}")
set(CMAKE_C_COMPILER_TARGET   x86_64-sie-ps5)
set(CMAKE_CXX_COMPILER_TARGET x86_64-sie-ps5)

# Everything is compiled into static archives; the executable link is a script.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Zen 2 cores: AVX2 is available on the PS5. IEEE denormals match the MXCSR
# the app CRT sets (PS5_Vulkan's tooling/prospero-clang18).
set(WOWEE_PS5_COMMON_FLAGS
    "-fPIC -ffunction-sections -fdata-sections -funwind-tables -fexceptions -march=znver2 -fvisibility-nodllstorageclass=default -fno-stack-protector -fno-plt -femulated-tls -fdenormal-fp-math=ieee -isysroot ${PS5_SDK} -D_GNU_SOURCE -DWOWEE_PS5=1 -include ${CMAKE_CURRENT_LIST_DIR}/../ps5/compat/ps5_lfs.h")
set(CMAKE_C_FLAGS_INIT   "${WOWEE_PS5_COMMON_FLAGS} -isystem ${PS5_SDK}/target/include")
set(CMAKE_CXX_FLAGS_INIT "${WOWEE_PS5_COMMON_FLAGS} -fcxx-exceptions -frtti -isystem ${PS5_SDK}/target/include/c++/v1 -isystem ${PS5_SDK}/target/include")

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_FIND_ROOT_PATH "${PS5_SDK}/target")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
