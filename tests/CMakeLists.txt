# Physics regression tests. One executable, one CTest entry per group (tag filter).
add_executable(qsig_tests
    main.cpp
    test_core.cpp
    test_fields.cpp
    test_solvers.cpp
    test_parity.cpp
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
qsig_add_test_group(validation     "[validation]"     slow physics)
qsig_add_test_group(parity         "[parity]"         slow parity)
