# qsig_physics: the physics core. Pure C++20, no Qt dependency, ever.
add_library(qsig_physics STATIC
    src/antenna_surface_geometry.cpp
    src/antenna_validation_bench.cpp
    src/dense_solver.cpp
    src/electrostatics.cpp
    src/em_calculator_model.cpp
    src/em_engineering_model.cpp
    src/em_units_calculator_model.cpp
    src/fdtd_analysis.cpp
    src/fdtd_solver.cpp
    src/hybrid_wire_surface_mom.cpp
    src/laplace_rail_model.cpp
    src/magnetostatics.cpp
    src/microstrip_models.cpp
    src/moment_method.cpp
    src/numerical_em_solvers.cpp
    src/pec_surface_mom.cpp
    src/sheet_integrals.cpp
    src/vna_time_domain.cpp
)
add_library(qsig::physics ALIAS qsig_physics)

target_include_directories(qsig_physics PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(qsig_physics PUBLIC cxx_std_20)
# The large dense solver shares its rows between threads.
find_package(Threads REQUIRED)
target_link_libraries(qsig_physics PUBLIC Threads::Threads)
qsig_set_warnings(qsig_physics)
qsig_set_fp_model(qsig_physics)

# Guard rail: the core must stay Qt-free. Fails the configure step if someone links Qt here.
get_target_property(_qsig_physics_libs qsig_physics LINK_LIBRARIES)
if(_qsig_physics_libs MATCHES "Qt[0-9]*::")
    message(FATAL_ERROR "qsig_physics must not depend on Qt")
endif()
