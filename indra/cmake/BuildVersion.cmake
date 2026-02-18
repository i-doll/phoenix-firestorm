# -*- cmake -*-
# Construct the viewer version number based on the indra/VIEWER_VERSION file

if (NOT DEFINED VIEWER_SHORT_VERSION) # will be true in indra/, false in indra/newview/
    set(VIEWER_VERSION_BASE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/newview/VIEWER_VERSION.txt")

    if ( EXISTS ${VIEWER_VERSION_BASE_FILE} )
        file(STRINGS ${VIEWER_VERSION_BASE_FILE} VIEWER_SHORT_VERSION REGEX "^[0-9]+\\.[0-9]+\\.[0-9]+")
        string(REGEX REPLACE "^([0-9]+)\\.[0-9]+\\.[0-9]+" "\\1" VIEWER_VERSION_MAJOR ${VIEWER_SHORT_VERSION})
        string(REGEX REPLACE "^[0-9]+\\.([0-9]+)\\.[0-9]+" "\\1" VIEWER_VERSION_MINOR ${VIEWER_SHORT_VERSION})
        string(REGEX REPLACE "^[0-9]+\\.[0-9]+\\.([0-9]+)" "\\1" VIEWER_VERSION_PATCH ${VIEWER_SHORT_VERSION})
        if (DEFINED ENV{revision})
           set(VIEWER_VERSION_REVISION $ENV{revision})
           message(STATUS "Revision (from environment): ${VIEWER_VERSION_REVISION}")

        elseif (DEFINED ENV{AUTOBUILD_BUILD_ID})
           set(VIEWER_VERSION_REVISION $ENV{AUTOBUILD_BUILD_ID})
           message(STATUS "Revision (from autobuild environment): ${VIEWER_VERSION_REVISION}")

        else (DEFINED ENV{revision})
            find_program(GIT git)

            # NOTE: this project typically has its top-level CMakeLists.txt in "indra/",
            # but the git repo root is one level above. Out-of-tree builds can otherwise
            # end up with a revision of 0.
            set(_git_workdir "${CMAKE_SOURCE_DIR}/..")
            if (NOT EXISTS "${_git_workdir}/.git")
                # Fallback: try the source dir itself.
                set(_git_workdir "${CMAKE_SOURCE_DIR}")
            endif()

            if (GIT AND NOT "${GIT}" MATCHES "-NOTFOUND$")
                execute_process(
                        COMMAND ${GIT} -C "${_git_workdir}" rev-list --count HEAD
                        OUTPUT_VARIABLE VIEWER_VERSION_REVISION
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET
                )
                if ("${VIEWER_VERSION_REVISION}" MATCHES "^[0-9]+$")
                    message(STATUS "Revision (from git) ${VIEWER_VERSION_REVISION}")
                else ("${VIEWER_VERSION_REVISION}" MATCHES "^[0-9]+$")
                    message(STATUS "Revision not set (git repo not found?); using 0")
                    set(VIEWER_VERSION_REVISION 0 )
                endif ("${VIEWER_VERSION_REVISION}" MATCHES "^[0-9]+$")

                # Provide a sane default for the git hash if the build environment didn't.
                if (NOT DEFINED VIEWER_VERSION_GITHASH OR "${VIEWER_VERSION_GITHASH}" STREQUAL "")
                    execute_process(
                            COMMAND ${GIT} -C "${_git_workdir}" rev-parse --short=10 HEAD
                            OUTPUT_VARIABLE VIEWER_VERSION_GITHASH
                            OUTPUT_STRIP_TRAILING_WHITESPACE
                            ERROR_QUIET
                    )
                endif()
            else (GIT AND NOT "${GIT}" MATCHES "-NOTFOUND$")
                message(STATUS "Revision not set: 'git' not found; using 0")
                set(VIEWER_VERSION_REVISION 0)
            endif (GIT AND NOT "${GIT}" MATCHES "-NOTFOUND$")
        endif (DEFINED ENV{revision})
        message(STATUS "Building '${VIEWER_CHANNEL}' Version ${VIEWER_SHORT_VERSION}.${VIEWER_VERSION_REVISION}")
    else ( EXISTS ${VIEWER_VERSION_BASE_FILE} )
        message(SEND_ERROR "Cannot get viewer version from '${VIEWER_VERSION_BASE_FILE}'") 
    endif ( EXISTS ${VIEWER_VERSION_BASE_FILE} )

    if ("${VIEWER_VERSION_REVISION}" STREQUAL "")
      message(STATUS "Ultimate fallback, revision was blank or not set: will use 0")
      set(VIEWER_VERSION_REVISION 0)
    endif ("${VIEWER_VERSION_REVISION}" STREQUAL "")

    # <FS:PP>
    set(VIEWER_VERSION_LL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/newview/VIEWER_VERSION_LL.txt")
    if (EXISTS ${VIEWER_VERSION_LL_FILE})
        file(STRINGS ${VIEWER_VERSION_LL_FILE} VIEWER_VERSION_LL LIMIT_COUNT 1)
        string(STRIP "${VIEWER_VERSION_LL}" VIEWER_VERSION_LL)
        message(STATUS "Upstream viewer version: ${VIEWER_VERSION_LL}")
    endif ()
    # </FS:PP>

    set(VIEWER_CHANNEL_VERSION_DEFINES
        "LL_VIEWER_CHANNEL=${VIEWER_CHANNEL}"
        "LL_VIEWER_VERSION_MAJOR=${VIEWER_VERSION_MAJOR}"
        "LL_VIEWER_VERSION_MINOR=${VIEWER_VERSION_MINOR}"
        "LL_VIEWER_VERSION_PATCH=${VIEWER_VERSION_PATCH}"
        "LL_VIEWER_VERSION_BUILD=${VIEWER_VERSION_REVISION}"
        "FS_VIEWER_VERSION_GITHASH=${VIEWER_VERSION_GITHASH}"
        "LLBUILD_CONFIG=\"${CMAKE_BUILD_TYPE}\""
        )
endif (NOT DEFINED VIEWER_SHORT_VERSION)
