cmake_minimum_required(VERSION 3.16)
project(linuxcad LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(OpenCASCADE REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)

add_executable(linuxcad
    src/main.cpp
    src/MainWindow.cpp
    src/OcctViewport.cpp
    src/StepImport.cpp
    src/StlExport.cpp
)

target_include_directories(linuxcad PRIVATE
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
    TKMesh
    TKService
    TKV3d
    TKOpenGl
    TKDESTEP
    TKDESTL
    TKXSBase
)
