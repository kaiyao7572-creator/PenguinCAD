cmake_minimum_required(VERSION 3.16)
project(penguincad LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(OpenCASCADE REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)

# Globbed so new subsystem files get picked up without editing this file
# (which would otherwise be a constant merge conflict between parallel
# workstreams). CONFIGURE_DEPENDS re-globs on each build.
file(GLOB_RECURSE PENGUINCAD_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE PENGUINCAD_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)

# The toolbar icons, compiled in by AUTORCC. The .qrc names every file, so
# a new SVG goes in there as well as in resources/icons/ (tools/make_icons.py
# writes both).
add_executable(penguincad ${PENGUINCAD_SOURCES} ${PENGUINCAD_HEADERS}
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons.qrc)

target_include_directories(penguincad PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCASCADE_INCLUDE_DIR}
)

target_link_directories(penguincad PRIVATE
    ${OpenCASCADE_LIBRARY_DIR}
)

target_link_libraries(penguincad PRIVATE
    Qt6::Widgets
    Qt6::OpenGLWidgets
    TKernel
    TKMath
    TKBRep
    TKGeomBase
    TKGeomAlgo
    TKG3d
    TKG2d
    TKShHealing
    TKTopAlgo
    TKPrim
    TKBO
    TKBool
    TKFillet
    TKOffset
    TKFeat
    TKMesh
    TKService
    TKV3d
    TKOpenGl
    TKDESTEP
    TKDESTL
    TKXSBase
    TKCAF
    TKLCAF
    TKVCAF
)

# ---- install: the binary plus what a desktop needs to find and launch it ----
#
# The .desktop file puts PenguinCAD in the application menu and search, the
# metainfo describes it to software centres, and the icon is the one both
# show. The Flatpak build (packaging/) relies on these same rules.
include(GNUInstallDirs)
set(PENGUINCAD_APP_ID io.github.kaiyao7572_creator.PenguinCAD)

install(TARGETS penguincad RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/${PENGUINCAD_APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/${PENGUINCAD_APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES packaging/${PENGUINCAD_APP_ID}.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES packaging/${PENGUINCAD_APP_ID}.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
