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
set(QSIG_LEGACY_DIR "" CACHE PATH "QTsignalApp 6.14 folder, to rebuild the legacy parity baseline (optional)")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# Layer 1 - physics core (no Qt). Phase 0.
add_subdirectory(physics)

# Layer 2 - application (QtCore) and layer 3 - interface arrive with phase 1:
#   add_subdirectory(app)
#   add_subdirectory(ui)

if(QSIG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(QSIG_LEGACY_DIR)
    add_subdirectory(tools/baseline)
endif()
