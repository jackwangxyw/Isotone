# isotone: the Qt 6 Quick UI (stage 4). QML for the interface, C++ for the model
# and the graph; links the core directly (docs/ui-spec.md, "Framework").
#
# The QML module is a static library, so the app and the QML tests load the same
# module.
# 6.4 is the floor because that is what Ubuntu 24.04 ships, and Linux Mint 22 is
# built on it: holding out for 6.11 would mean a .deb that cannot use the
# distribution's Qt. Windows still builds against 6.11. What 6.4 lacks is bridged
# where it is used (qmlsingleton.h, the import path in main.cpp, Theme.qml's
# system scheme, Icon.qml's renderer, DropOverlay.qml's outline).
find_package(Qt6 6.4 REQUIRED COMPONENTS Gui Network Qml Quick QuickTest Test Widgets)
if(NOT WIN32)
    # Linux: the GlobalShortcuts portal on Wayland (globalhotkeys_posix.cpp).
    find_package(Qt6 REQUIRED COMPONENTS DBus)
endif()
# The policy level is the Qt in use, not the floor. Passing a version lower than
# the installed Qt quietly changes what qt_add_qml_module does: with 6.11 present
# and REQUIRES 6.4, the module's types stop registering and every QML test fails
# with "DefaultOutputFollower is not a type" while the app exits at startup.
qt_standard_project_setup(REQUIRES ${Qt6_VERSION})

# The backend's platform half. Classified by compiling each file against Linux
# rather than by reading the includes: these six are the ones that do not build,
# and the five below do.
#
# speaker_setup is the near miss and says where the real work is. Nothing in it
# touches Win32 except one function, save_speaker_setup, whose path is a
# std::wstring going into windows/transport's persisted_state.h. The wide-string
# identity leaks into otherwise portable code; that seam, not windows.h, is what
# stage 4 costs.
if(WIN32)
    set(ISOTONE_UI_BACKEND_PLATFORM
        backend/devicelink.cpp
        backend/devicelink.h
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
    set(ISOTONE_UI_BACKEND_PLATFORM_LIBS isotone_transport isotone_compat isotone_devices)
else()
    # devicelink_posix is where edits go on Linux: the daemon's region in place of
    # the APO's mapping, and the daemon's ring in place of the loopback capture.
    # PipeWire's registry stands in for the device APIs, XDG autostart for the
    # Run key, and a PipeWire stream into Isotone's sink for the tones.
    set(ISOTONE_UI_BACKEND_PLATFORM
        backend/devicelink.h
        backend/devicelink_posix.cpp
        backend/autostart_xdg.h
        backend/autostart_xdg.cpp
        # Which sink the daemon feeds, and the format it runs at.
        backend/daemon_region.h
        backend/daemon_region.cpp
    )
    set(ISOTONE_UI_BACKEND_PLATFORM_LIBS isotone_transport_posix)
    # Enumerating outputs needs PipeWire itself, which the top-level list file
    # has already looked for. Without it the backend still builds, just without
    # a way to see the machine's sinks.
    if(PIPEWIRE_FOUND)
        # The tones play into Isotone's virtual sink through PipeWire.
        list(APPEND ISOTONE_UI_BACKEND_PLATFORM
             backend/pipewire_outputs.h
             backend/pipewire_outputs.cpp
             backend/test_tone.h
             backend/test_tone_posix.cpp)
        list(APPEND ISOTONE_UI_BACKEND_PLATFORM_LIBS PkgConfig::PIPEWIRE)
    endif()
endif()

# Without Qt: spectrum analysis, the state an output gets, and where edits go.
add_library(isotone_ui_backend STATIC
    backend/output_state.cpp
    backend/output_state.h
    backend/spectrum.cpp
    backend/spectrum.h
    backend/typed_value.cpp
    backend/typed_value.h
    # Surround; saves through the platform's persisted state.
    backend/speaker_setup.cpp
    backend/speaker_setup.h
    # Surround
    backend/pink_noise.cpp
    backend/pink_noise.h
    # EQ by ear
    backend/sine_tone.cpp
    backend/sine_tone.h
    ${ISOTONE_UI_BACKEND_PLATFORM}
)
target_include_directories(isotone_ui_backend PUBLIC backend)
target_link_libraries(isotone_ui_backend PUBLIC isotone_core ${ISOTONE_UI_BACKEND_PLATFORM_LIBS})
if(WIN32)
    # windows.h without the min and max macros, which break std::min and std::max.
    target_compile_definitions(isotone_ui_backend PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    # Settings: file versions for About.
    target_link_libraries(isotone_ui_backend PRIVATE version)
endif()

if(NOT WIN32)
    # What the Linux backend can be held to without a Qt layer or a daemon: the
    # test creates the sink's region and writes its ring the way a daemon does.
    if(ISOTONE_BUILD_TESTS)
        set(ISOTONE_UI_POSIX_TESTS tests/test_main.cpp tests/test_devicelink_posix.cpp
                                   tests/test_autostart_xdg.cpp)
        if(PIPEWIRE_FOUND)
            list(APPEND ISOTONE_UI_POSIX_TESTS tests/test_pipewire_outputs.cpp)
        endif()
        add_executable(ui_backend_posix_tests ${ISOTONE_UI_POSIX_TESTS})
        target_link_libraries(ui_backend_posix_tests PRIVATE isotone_ui_backend)
        target_include_directories(ui_backend_posix_tests PRIVATE
                                   ${PROJECT_SOURCE_DIR}/core/tests/third_party)
        add_test(NAME ui_backend_posix_tests COMMAND ui_backend_posix_tests)
    endif()
    # The Qt layer lists outputs and plays its tones through PipeWire; without
    # it there is no Linux app to build.
    if(NOT PIPEWIRE_FOUND)
        message(STATUS "isotone UI: libpipewire-0.3 not found, only the backend is built")
        return()
    endif()
endif()

# The Qt layer's platform half. Linux has one engine, the daemon, and nothing to
# install: Devices lists sinks, Devicetool only starts the daemon, and global
# hotkeys are X11 grabs or the GlobalShortcuts portal.
if(WIN32)
    set(ISOTONE_UI_PLATFORM
        # Devices work package.
        src/devicesmodel.cpp
        src/devicestatus.cpp
        src/devicestatus.h
        src/devicetoolcontroller.cpp
        src/devicetoolcontroller.h
        src/devicetoolrunner.cpp
        src/devicetoolrunner.h
        src/equalizerapoconfig.cpp
        src/equalizerapoconfig.h
        # Settings
        src/globalhotkeys.cpp
    )
else()
    set(ISOTONE_UI_PLATFORM
        src/devicesmodel_posix.cpp
        src/devicetool_posix.cpp
        src/devicetool_posix.h
        src/globalhotkeys_posix.cpp
    )
endif()

qt_add_library(isotone_ui STATIC
    src/apppaths.cpp
    src/apppaths.h
    src/appsettings.cpp
    src/appsettings.h
    src/eqsession.cpp
    src/eqsession.h
    # EQ by ear
    src/eqbyear.cpp
    src/eqbyear.h
    src/filterglyph.cpp
    src/filterglyph.h
    src/outputs.cpp
    src/outputs.h
    src/presets.cpp
    src/presets.h
    src/qmlsingleton.h
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
    src/devicesmodel.h
    # Settings
    src/about.cpp
    src/about.h
    src/bandkeys.cpp
    src/bandkeys.h
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
    ${ISOTONE_UI_PLATFORM}
)
# Settings, About: the version, defined once in project().
target_compile_definitions(isotone_ui PRIVATE ISOTONE_VERSION="${PROJECT_VERSION}")

# Before qt_add_qml_module, which reads it.
set_source_files_properties(qml/Theme.qml qml/UiState.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(qml/PresetActions.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(qml/GeneralSettings.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)   # Settings

qt_add_qml_module(isotone_ui
    URI Isotone
    # Qt 6.4 requires it; 6.11 made it optional.
    VERSION 1.0
    # 6.5 made /qt/qml the default (QTP0001); 6.4 puts the module at /, and the
    # fonts and Main.qml are loaded by these paths.
    RESOURCE_PREFIX /qt/qml
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
        qml/SettingSlider.qml
        qml/SettingsRow.qml
        qml/SettingsSection.qml
        qml/WindowKeys.qml
        qml/PressWatch.qml
        qml/WindowClose.qml
        # EQ by ear
        qml/EarView.qml
        qml/EarBandList.qml
        qml/SweepSlider.qml
        qml/VolumeDialog.qml
    RESOURCES
        fonts/InstrumentSans-Regular.ttf
        fonts/InstrumentSans-Medium.ttf
        fonts/InstrumentSans-SemiBold.ttf
)

target_include_directories(isotone_ui PUBLIC src)
target_link_libraries(isotone_ui PUBLIC Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Widgets isotone_ui_backend)
# Settings: the single instance's local socket.
target_link_libraries(isotone_ui PUBLIC Qt6::Network)
if(WIN32)
    # C4702 (unreachable code) is reported on lines of Qt's qmetatype.h, qvariant.h
    # and qjsengine.h as their templates are instantiated. It comes from code
    # generation, which /external:W0 does not cover.
    target_compile_options(isotone_ui PUBLIC /wd4702)
    # Devices: the elevated devicetool session, and the devicetool this build makes,
    # found when isotone.exe has none beside it.
    target_link_libraries(isotone_ui PUBLIC isotone_devicetool_session)
    target_compile_definitions(isotone_ui PRIVATE ISOTONE_DEVICETOOL_BUILD_PATH="$<TARGET_FILE:isotone-devicetool>")
    add_dependencies(isotone_ui isotone-devicetool)

    # res/isotone.rc: the exe's icon.
    qt_add_executable(isotone WIN32 src/main.cpp res/isotone.rc)
else()
    # Global hotkeys: X11 key grabs, and the portal over D-Bus.
    find_package(X11 REQUIRED)
    target_link_libraries(isotone_ui PUBLIC Qt6::DBus X11::X11 X11::xcb)
    qt_add_executable(isotone src/main.cpp)
endif()
target_link_libraries(isotone PRIVATE isotone_ui isotone_uiplugin)

if(ISOTONE_BUILD_TESTS AND WIN32)
    add_executable(ui_tests tests/test_main.cpp tests/test_backend.cpp tests/test_typed_value.cpp
                   tests/test_speaker_setup.cpp tests/test_config_attach.cpp tests/test_settings_backend.cpp
                   tests/test_compat_retry.cpp tests/test_sine_tone.cpp)
    target_link_libraries(ui_tests PRIVATE isotone_ui_backend)
    target_include_directories(ui_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests/third_party)
    target_compile_options(ui_tests PRIVATE /wd5285)   # doctest, as in core/tests
    add_test(NAME ui_tests COMMAND ui_tests)

    add_executable(ui_model_tests tests/test_model.cpp tests/test_speakers_model.cpp tests/test_undo.cpp tests/test_presets.cpp
                   tests/test_devices.cpp tests/test_settings.cpp tests/test_session_fixes.cpp
                   tests/test_spectrum_display.cpp tests/test_eqbyear.cpp)
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
