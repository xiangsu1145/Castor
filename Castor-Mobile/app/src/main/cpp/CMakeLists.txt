cmake_minimum_required(VERSION 3.22)
project(screenbridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# libjpeg-turbo 的 CMake 工程禁止 add_subdirectory 集成，
# 因此静态库用 NDK 单独构建（Release + NEON SIMD）后放在 third_party/prebuilt。
# 重建方法见 docs/pc-env-check.md 同级目录下的说明。
set(TJ_PREBUILT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/prebuilt)

add_library(turbojpeg STATIC IMPORTED)
set_target_properties(turbojpeg PROPERTIES
    IMPORTED_LOCATION ${TJ_PREBUILT}/arm64-v8a/libturbojpeg.a
    INTERFACE_INCLUDE_DIRECTORIES ${TJ_PREBUILT}/include)

add_library(ndi SHARED IMPORTED)
set_target_properties(ndi PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libndi.so
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(screenbridge SHARED screenbridge.cpp)
target_link_libraries(screenbridge PRIVATE turbojpeg ndi log)
