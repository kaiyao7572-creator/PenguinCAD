cmake_minimum_required(VERSION 3.16)
project(linuxcad LANGUAGES CXX)

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
file(GLOB_RECURSE LINUXCAD_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE LINUXCAD_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)

add_executable(linuxcad ${LINUXCAD_SOURCES} ${LINUXCAD_HEADERS})

target_include_directories(linuxcad PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCASCADE_INCLUDE_DIR}
)

target_link_directories(linuxcad PRIVATE
    ${OpenCASCADE_LIBRARY_DIR}
)

target_link_libraries(linuxcad PRIVATE
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
