cmake_minimum_required(VERSION 3.22.1)
project("loader")

set(CMAKE_CXX_STANDARD 23)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_definitions(-DZKSU_VERSION=\"${ZKSU_VERSION}\")

aux_source_directory(common COMMON_SRC_LIST)
add_library(common STATIC ${COMMON_SRC_LIST})
target_include_directories(common PRIVATE include)
target_link_libraries(common log)

aux_source_directory(injector INJECTOR_SRC_LIST)
add_library(zygisk SHARED ${INJECTOR_SRC_LIST})
target_compile_options(zygisk PRIVATE -Wno-invalid-offsetof -Wno-unused-private-field)
target_include_directories(zygisk PRIVATE include)
target_link_libraries(zygisk log common lsplt_static)

aux_source_directory(ptracer PTRACER_SRC_LIST)
add_executable(libzygisk_ptrace.so ${PTRACER_SRC_LIST})
target_include_directories(libzygisk_ptrace.so PRIVATE include)
target_link_libraries(libzygisk_ptrace.so log common)

# Both the library and the PIE executable must load on 16 KiB kernels. The
# common-page-size flag also keeps RELRO from covering adjacent writable data.
if(ANDROID_ABI STREQUAL "arm64-v8a" OR ANDROID_ABI STREQUAL "x86_64")
    foreach(target zygisk libzygisk_ptrace.so)
        target_link_options(${target} PRIVATE
            "-Wl,-z,max-page-size=16384"
            "-Wl,-z,common-page-size=16384")
    endforeach()
endif()

add_subdirectory(external)
