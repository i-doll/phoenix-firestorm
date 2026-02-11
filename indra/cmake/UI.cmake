# -*- cmake -*-
include(Prebuilt)
include(FreeType)
include(GLIB)

add_library( ll::uilibraries INTERFACE IMPORTED )

if (LINUX)
  target_link_libraries( ll::uilibraries INTERFACE
          glib-2.0
          gmodule-2.0
          gobject-2.0
          gthread-2.0
          ll::freetype
          )
  target_include_directories( ll::uilibraries SYSTEM INTERFACE
          ${GLIB_INCLUDE_DIRS}
          )
endif (LINUX)
if( WINDOWS )
  target_link_libraries( ll::uilibraries INTERFACE
          opengl32
          comdlg32
          dxguid
          kernel32
          odbc32
          odbccp32
          oleaut32
          shell32
          Vfw32
          wer
          winspool
          imm32
          )
endif()

target_include_directories( ll::uilibraries SYSTEM INTERFACE
        ${LIBS_PREBUILT_DIR}/include
        )

