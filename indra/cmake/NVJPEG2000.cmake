# -*- cmake -*-
# NVIDIA nvJPEG2000 CUDA-accelerated JPEG2000 decoder
# Only available on Linux with CUDA Toolkit 11.0+

include_guard()

option(USE_NVJPEG2000 "Use NVIDIA nvJPEG2000 CUDA decoder" OFF)

add_library(ll::nvjpeg2k INTERFACE IMPORTED)

if (USE_NVJPEG2000)
    if (NOT LINUX)
        message(FATAL_ERROR "USE_NVJPEG2000 is only supported on Linux")
    endif()

    # Find CUDA Toolkit (requires 11.0+)
    find_package(CUDAToolkit 11.0 REQUIRED)

    # Find nvJPEG2000 library
    find_library(NVJPEG2K_LIBRARY
        NAMES nvjpeg2k
        PATHS
            ${CUDAToolkit_LIBRARY_DIR}
            /usr/local/cuda/lib64
            /usr/lib/x86_64-linux-gnu
        REQUIRED
    )

    find_path(NVJPEG2K_INCLUDE_DIR
        NAMES nvjpeg2k.h
        PATHS
            ${CUDAToolkit_INCLUDE_DIRS}
            /usr/local/cuda/include
            /usr/include
        REQUIRED
    )

    message(STATUS "Found nvJPEG2000: ${NVJPEG2K_LIBRARY}")
    message(STATUS "Found nvJPEG2000 include: ${NVJPEG2K_INCLUDE_DIR}")

    target_link_libraries(ll::nvjpeg2k INTERFACE
        ${NVJPEG2K_LIBRARY}
        CUDA::cudart
        CUDA::cuda_driver
    )

    target_include_directories(ll::nvjpeg2k SYSTEM INTERFACE
        ${NVJPEG2K_INCLUDE_DIR}
        ${CUDAToolkit_INCLUDE_DIRS}
    )

    target_compile_definitions(ll::nvjpeg2k INTERFACE LL_NVJPEG2K=1)

    message(STATUS "nvJPEG2000 CUDA decoder enabled")
else()
    message(STATUS "nvJPEG2000 CUDA decoder disabled")
endif()
