cmake_minimum_required(VERSION 3.21)

project(QTsignalApp
    VERSION 7.0.0
    DESCRIPTION "Electrostatics, magnetostatics, 2D FDTD and antenna modelling for teaching and RF development"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(QSIG_BUILD_TESTS "Build the physics regression tests" ON)
option(QSIG_BUILD_APP "Build the Qt Quick application (needs Qt 6.8 or later)" ON)
set(QSIG_LEGACY_DIR "" CACHE PATH "QTsignalApp 6.14 folder, to rebuild the legacy parity baseline (optional)")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# Layer 1 - physics core (no Qt). Phase 0.
add_subdirectory(physics)

# Layer 2 - application logic (QtCore/QtGui) and layer 3 - Qt Quick interface.
# Without Qt (CI runners, physics-only work) the physics core and its tests still build.
if(QSIG_BUILD_APP)
    find_package(Qt6 6.8 QUIET COMPONENTS Core Gui Concurrent Qml Quick QuickControls2)
    if(Qt6_FOUND)
        qt_standard_project_setup(REQUIRES 6.8)
        add_subdirectory(app)
        add_subdirectory(ui)
        message(STATUS "QTsignalApp: Qt ${Qt6_VERSION} found, building the application")
    else()
        message(STATUS "QTsignalApp: Qt 6.8+ not found, building the physics core and tests only")
    endif()
endif()

if(QSIG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(QSIG_LEGACY_DIR)
    add_subdirectory(tools/baseline)
endif()
