# Layer 3 - the Qt Quick interface (executable QTsignalApp).
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(QTsignalApp main.cpp)
qt_add_qml_module(QTsignalApp
    URI Qsig
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/Sidebar.qml
        qml/ScenePanel.qml
        qml/FieldViewport.qml
        qml/Inspector.qml
        qml/Legend.qml
        qml/QuantityField.qml
        qml/SectionTitle.qml
        qml/PanelIcon.qml
        qml/FoldButton.qml
        qml/EdgeTab.qml
        qml/ChoiceField.qml
        qml/TraceChart.qml
        qml/ProfileChart.qml
        qml/AntennaViewport.qml
        qml/FloatingPanel.qml
        qml/RibbonGroup.qml
        qml/RibbonButton.qml
        qml/RibbonIcon.qml
    SOURCES
        field_view.h
        field_view.cpp
        field_view3d.h
        field_view3d.cpp
        orbit_navigator.h
        orbit_navigator.cpp
        antenna_views.h
        antenna_views.cpp
)
target_link_libraries(QTsignalApp PRIVATE qsig_app Qt6::Quick Qt6::QuickControls2 Qt6::Qml)
# Qt's generated QML-cache loader trips GCC's -Wnull-dereference inside qhash.h (inlined Qt code):
# not ours, so silenced for that generated file only.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/.rcc/qmlcache/QTsignalApp_qmlcache_loader.cpp"
        PROPERTIES COMPILE_OPTIONS "-Wno-null-dereference")
endif()
target_compile_definitions(QTsignalApp PRIVATE QSIG_VERSION="${PROJECT_VERSION}")
qsig_set_warnings(QTsignalApp)
