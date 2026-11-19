# Physics regression tests. One executable, one CTest entry per group (tag filter).
add_executable(qsig_tests
    main.cpp
    test_core.cpp
    test_fields.cpp
    test_solvers.cpp
    test_parity.cpp
    test_exact_sheets.cpp
    test_surfaces.cpp
)
target_link_libraries(qsig_tests PRIVATE qsig::physics)
target_include_directories(qsig_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
qsig_set_warnings(qsig_tests)

function(qsig_add_test_group name filter)
    add_test(NAME ${name} COMMAND qsig_tests "${filter}")
    set_tests_properties(${name} PROPERTIES LABELS "${ARGN}")
endfunction()

qsig_add_test_group(constants      "[constants]"      fast)
qsig_add_test_group(core           "[core]"           fast)
qsig_add_test_group(linalg         "[linalg]"         fast)
qsig_add_test_group(electrostatics "[electrostatics]" fast physics)
qsig_add_test_group(magnetostatics "[magnetostatics]" fast physics)
qsig_add_test_group(fdtd           "[fdtd]"           physics)
qsig_add_test_group(mom            "[mom]"            physics)
qsig_add_test_group(surfaces       "[surfaces]"       physics)
qsig_add_test_group(validation     "[validation]"     slow physics)
qsig_add_test_group(parity         "[parity]"         slow parity)

# Application layer (needs Qt): units, scene/undo, persistence, background field map.
if(TARGET qsig_app)
    add_executable(qsig_app_tests app_main.cpp test_app.cpp test_waves.cpp test_antennas.cpp test_view3d.cpp)
    target_link_libraries(qsig_app_tests PRIVATE qsig_app)
    target_include_directories(qsig_app_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    qsig_set_warnings(qsig_app_tests)
    add_test(NAME app COMMAND qsig_app_tests "[app]")
    set_tests_properties(app PROPERTIES LABELS "fast;app")
    # FDTD simulation of the "Ondes" module against physics.
    add_test(NAME waves COMMAND qsig_app_tests "[waves]")
    set_tests_properties(waves PROPERTIES LABELS "app;physics")
    # Wire antennas of the "Antennes" module (method of moments, far field, sweeps).
    add_test(NAME antennas COMMAND qsig_app_tests "[antennas]")
    set_tests_properties(antennas PROPERTIES LABELS "app;physics")
    # 3D views: orbit camera and the shapes of the sources in space.
    add_test(NAME view3d COMMAND qsig_app_tests "[view3d]")
    set_tests_properties(view3d PROPERTIES LABELS "fast;app")
    if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
        # Run against the Qt the build used, without a deployment step.
        get_target_property(_qsig_qmake Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(_qsig_qt_bin "${_qsig_qmake}" DIRECTORY)
        set_tests_properties(app waves antennas view3d PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qsig_qt_bin}")
    endif()
endif()
