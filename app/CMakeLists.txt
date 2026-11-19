# Layer 2 - application logic (QtCore / QtGui, no widgets, no QML): scene model, undo,
# persistence, background field computation. Testable without any window.
qt_add_library(qsig_app STATIC
    include/qsig/app/view_plane.h
    include/qsig/app/live_frame.h
    include/qsig/app/source_outline.h
    include/qsig/app/source_shapes.h
    include/qsig/app/orbit_camera.h
    include/qsig/app/scene_base.h
    include/qsig/app/electrostatic_scene.h
    include/qsig/app/magnetostatic_scene.h
    include/qsig/app/wave_scene.h
    include/qsig/app/wave_simulation.h
    include/qsig/app/antenna_model.h
    include/qsig/app/antenna_optimizer.h
    include/qsig/app/antenna_bench.h
    include/qsig/app/antenna_scene.h
    include/qsig/app/field_lines.h
    include/qsig/app/field_map.h
    include/qsig/app/units.h
    src/scene_base.cpp
    src/source_shapes.cpp
    src/electrostatic_scene.cpp
    src/magnetostatic_scene.cpp
    src/wave_scene.cpp
    src/wave_simulation.cpp
    src/antenna_model.cpp
    src/antenna_optimizer.cpp
    src/antenna_bench.cpp
    src/antenna_scene.cpp
    src/outline_builder.h
    src/field_lines.cpp
    src/field_map.cpp
    src/units.cpp
    src/unit_prefixes.h
)
target_include_directories(qsig_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(Threads REQUIRED) # std::thread in the antenna far field
target_link_libraries(qsig_app PUBLIC qsig::physics Qt6::Core Qt6::Gui Qt6::Concurrent Threads::Threads)
qsig_set_warnings(qsig_app)
