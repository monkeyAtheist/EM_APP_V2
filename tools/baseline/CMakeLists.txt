# Optional: rebuilds the legacy-baseline recorder from the QTsignalApp 6.14 sources.
#   cmake --preset release -DQSIG_LEGACY_DIR=<path to QTsignalApp>
#   cmake --build --preset release --target legacy_baseline
#   build/release/tools/baseline/legacy_baseline > tests/fixtures/legacy_baseline.inc
# Only needed if the scenarios in tests/fixtures/scenarios.h change.
set(_legacy_sources
    electrostatic_model magnetostatic_model fdtd_solver numerical_em_solvers antenna_validation_bench
    hybrid_wire_surface_mom pec_surface_mom antenna_surface_geometry microstrip_models em_calculator_model)
list(TRANSFORM _legacy_sources PREPEND "${QSIG_LEGACY_DIR}/src/")
list(TRANSFORM _legacy_sources APPEND ".cpp")

add_executable(legacy_baseline legacy_baseline.cpp ${_legacy_sources})
# The Qt stand-in comes first so <QObject>, <QString>... resolve to it; no Qt needed.
target_include_directories(legacy_baseline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/qtshim
    ${QSIG_LEGACY_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests/fixtures)
target_compile_features(legacy_baseline PRIVATE cxx_std_20)
qsig_set_fp_model(legacy_baseline)
