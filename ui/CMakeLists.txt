# isotone: the Qt 6 Quick UI (stage 4). QML for the interface, C++ for the model
# and the graph; links the core directly (docs/ui-spec.md, "Framework").
#
# The QML module is a static library, so the app and the QML tests load the same
# module.
find_package(Qt6 6.11 REQUIRED COMPONENTS Gui Network Qml Quick QuickTest Test Widgets)
qt_standard_project_setup(REQUIRES 6.11)

# Without Qt: spectrum analysis, the state an output gets, and where edits go.
add_library(isotone_ui_backend STATIC
    backend/devicelink.cpp
    backend/devicelink.h
    backend/output_state.cpp
    backend/output_state.h
    backend/spectrum.cpp
    backend/spectrum.h
    backend/typed_value.cpp
    backend/typed_value.h
    # Surround
    backend/pink_noise.cpp
    backend/pink_noise.h
    backend/speaker_setup.cpp
    backend/speaker_setup.h
    backend/test_tone.cpp
    backend/test_tone.h
    # Devices work package.
    backend/config_attach.cpp
    backend/config_attach.h
    # Settings
    backend/diagnostics.cpp
    backend/diagnostics.h
    backend/startup_registration.cpp
    backend/startup_registration.h
)
target_include_directories(isotone_ui_backend PUBLIC backend)
# windows.h without the min and max macros, which break std::min and std::max.
target_compile_definitions(isotone_ui_backend PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(isotone_ui_backend PUBLIC isotone_core isotone_transport isotone_compat isotone_devices)
# Settings: file versions for About.
target_link_libraries(isotone_ui_backend PRIVATE version)

qt_add_library(isotone_ui STATIC
    src/apppaths.cpp
    src/apppaths.h
    src/appsettings.cpp
    src/appsettings.h
    src/eqsession.cpp
    src/eqsession.h
    src/filterglyph.cpp
    src/filterglyph.h
    src/outputs.cpp
    src/outputs.h
    src/presets.cpp
    src/presets.h
    src/responsegraph.cpp
    src/responsegraph.h
    # Surround
    src/speakers.cpp
    src/speakers.h
    src/speakerstore.cpp
    src/speakerstore.h
    # Presets work package.
    src/presetstore.cpp
    src/presetstore.h
    src/importpreview.cpp
    src/importpreview.h
    src/curvepreview.cpp
    src/curvepreview.h
    # Devices work package.
    src/devicesmodel.cpp
    src/devicesmodel.h
    src/devicestatus.cpp
    src/devicestatus.h
    src/devicetoolcontroller.cpp
    src/devicetoolcontroller.h
    src/devicetoolrunner.cpp
    src/devicetoolrunner.h
    src/equalizerapoconfig.cpp
    src/equalizerapoconfig.h
    # Settings
    src/about.cpp
    src/about.h
    src/bandkeys.cpp
    src/bandkeys.h
    src/globalhotkeys.cpp
    src/globalhotkeys.h
    src/logomark.cpp
    src/logomark.h
    src/previewsession.cpp
    src/previewsession.h
    src/shortcutregistry.cpp
    src/shortcutregistry.h
    src/singleinstance.cpp
    src/singleinstance.h
    src/startup.cpp
    src/startup.h
    src/traymenu.cpp
    src/traymenu.h
)
# Settings, About: the version, defined once in project().
target_compile_definitions(isotone_ui PRIVATE ISOTONE_VERSION="${PROJECT_VERSION}")

# Before qt_add_qml_module, which reads it.
set_source_files_properties(qml/Theme.qml qml/UiState.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(qml/PresetActions.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(qml/GeneralSettings.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)   # Settings

qt_add_qml_module(isotone_ui
    URI Isotone
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/Icon.qml
        qml/Toggle.qml
        qml/Segmented.qml
        qml/Sidebar.qml
        qml/TopBar.qml
        qml/GraphCard.qml
        qml/BandStrip.qml
        qml/BandColumn.qml
        qml/GainSlider.qml
        qml/ChannelsPanel.qml
        qml/ValueField.qml
        qml/BandMenu.qml
        qml/UiState.qml
        qml/Button.qml
        qml/TextBox.qml
        qml/DialogFrame.qml
        qml/Popover.qml
        qml/Spinner.qml
        qml/Toast.qml
        qml/StatusDot.qml
        qml/OutputList.qml
        qml/EqualizerView.qml
        qml/SpeakersView.qml
        qml/DevicesView.qml
        qml/SettingsView.qml
        qml/SettingsGeneral.qml
        qml/SettingsOutputs.qml
        qml/SettingsAppearance.qml
        qml/SettingsShortcuts.qml
        qml/SettingsAbout.qml
        qml/PresetsMenu.qml
        qml/SpeakersPanel.qml
        qml/TargetChips.qml
        qml/ShowingPicker.qml
        qml/LayoutDialog.qml
        qml/NewGroupDialog.qml
        qml/PresetActions.qml
        qml/PresetPrompts.qml
        qml/UnsavedDialog.qml
        qml/SaveAsDialog.qml
        qml/ImportDialog.qml
        qml/ExportDialog.qml
        qml/DropOverlay.qml
        # Devices work package.
        qml/DeviceDetail.qml
        qml/OperationBox.qml
        qml/UninstallDialog.qml
        qml/ReplaceDialog.qml
        qml/AttachDialog.qml
        qml/OutputSetupTable.qml
        qml/FirstRun.qml
        # Settings
        qml/AppShortcuts.qml
        qml/ColourPicker.qml
        qml/DefaultOutputFollower.qml
        qml/GeneralSettings.qml
        qml/KeyCaps.qml
        qml/PreviewGraph.qml
        qml/SettingBox.qml
        qml/SettingsRow.qml
        qml/SettingsSection.qml
        qml/WindowKeys.qml
        qml/PressWatch.qml
        qml/WindowClose.qml
    RESOURCES
        fonts/InstrumentSans-Regular.ttf
        fonts/InstrumentSans-Medium.ttf
        fonts/InstrumentSans-SemiBold.ttf
)

target_include_directories(isotone_ui PUBLIC src)
# C4702 (unreachable code) is reported on lines of Qt's qmetatype.h, qvariant.h
# and qjsengine.h as their templates are instantiated. It comes from code
# generation, which /external:W0 does not cover.
target_compile_options(isotone_ui PUBLIC /wd4702)
target_link_libraries(isotone_ui PUBLIC Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Widgets isotone_ui_backend)
# Devices: the elevated devicetool session, and the devicetool this build makes,
# found when isotone.exe has none beside it.
target_link_libraries(isotone_ui PUBLIC isotone_devicetool_session)
target_compile_definitions(isotone_ui PRIVATE ISOTONE_DEVICETOOL_BUILD_PATH="$<TARGET_FILE:isotone-devicetool>")
add_dependencies(isotone_ui isotone-devicetool)
# Settings: the single instance's local socket.
target_link_libraries(isotone_ui PUBLIC Qt6::Network)

qt_add_executable(isotone WIN32 src/main.cpp)
target_link_libraries(isotone PRIVATE isotone_ui isotone_uiplugin)

if(ISOTONE_BUILD_TESTS)
    add_executable(ui_tests tests/test_main.cpp tests/test_backend.cpp tests/test_typed_value.cpp
                   tests/test_speaker_setup.cpp tests/test_config_attach.cpp tests/test_settings_backend.cpp)
    target_link_libraries(ui_tests PRIVATE isotone_ui_backend)
    target_include_directories(ui_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests/third_party)
    target_compile_options(ui_tests PRIVATE /wd5285)   # doctest, as in core/tests
    add_test(NAME ui_tests COMMAND ui_tests)

    add_executable(ui_model_tests tests/test_model.cpp tests/test_speakers_model.cpp tests/test_undo.cpp tests/test_presets.cpp
                   tests/test_devices.cpp tests/test_settings.cpp tests/test_session_fixes.cpp)
    target_link_libraries(ui_model_tests PRIVATE isotone_ui Qt6::Test)
    target_compile_definitions(ui_model_tests PRIVATE ISOTONE_UI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/devices")
    target_include_directories(ui_model_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests/third_party)
    target_compile_options(ui_model_tests PRIVATE /wd5285)   # doctest, as in core/tests

    # QML tests: Qt Quick Test cases in tests/qml driving the real components.
    qt_add_executable(ui_qml_tests tests/qml_main.cpp)
    target_link_libraries(ui_qml_tests PRIVATE isotone_ui isotone_uiplugin Qt6::QuickTest)
    target_compile_definitions(ui_qml_tests PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml")
    # Devices work package: the outputs and devicetool the QML tests see.
    target_compile_definitions(ui_qml_tests PRIVATE ISOTONE_FAKE_DEVICETOOL_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tests/devices/fake-devicetool.json")
endif()
