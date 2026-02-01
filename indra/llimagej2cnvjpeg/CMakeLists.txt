# -*- cmake -*-

project(llimagej2cnvjpeg)

# Only build when USE_NVJPEG2000 is enabled
if (USE_NVJPEG2000)

include(00-Common)
include(LLCommon)
include(LLImage)
include(OpenJPEG)
include(NVJPEG2000)

set(llimagej2cnvjpeg_SOURCE_FILES
    llcudacontext.cpp
    llimagej2cnvjpeg.cpp
    )

set(llimagej2cnvjpeg_HEADER_FILES
    CMakeLists.txt

    llcudacontext.h
    llimagej2cnvjpeg.h
    )

list(APPEND llimagej2cnvjpeg_SOURCE_FILES ${llimagej2cnvjpeg_HEADER_FILES})

add_library(llimagej2cnvjpeg ${llimagej2cnvjpeg_SOURCE_FILES})

target_include_directories(llimagej2cnvjpeg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(llimagej2cnvjpeg
    llcommon
    llimage
    llimagej2coj
    ll::openjpeg
    ll::nvjpeg2k
    )

endif()
