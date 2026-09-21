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
        # Settings: launch at sign-in inside a Flatpak, which only the Background
        # portal can set.
        src/startup_portal.cpp
        src/startup_portal.h
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
        qml/OutputPicker.qml
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
    # And the version Explorer's properties page and Settings, About show.
    isotone_version_resource(isotone "Isotone")
else()
    # Global hotkeys: X11 key grabs, and the portal over D-Bus.
    find_package(X11 REQUIRED)
    target_link_libraries(isotone_ui PUBLIC Qt6::DBus X11::X11 X11::xcb)
    qt_add_executable(isotone src/main.cpp)
endif()
target_link_libraries(isotone PRIVATE isotone_ui isotone_uiplugin)

# ---------------------------------------------------------------------------
# Installing (stage 6)

install(TARGETS isotone RUNTIME DESTINATION "${ISOTONE_INSTALL_BINDIR}" COMPONENT app)

if(WIN32)
    # The Qt runtime, staged by windeployqt into the install directory rather
    # than listed here: which DLLs and QML modules an isotone.exe needs is a
    # question about the binary, and windeployqt reads it from the binary.
    #
    # --qmldir is what makes it stage the Qt QML modules this app imports; without
    # it the app starts and shows an empty window, because QtQuick's own plugins
    # are missing. --no-translations and --no-system-d3d-compiler keep about
    # 20 MB out of the installer that nothing here uses.
    find_program(ISOTONE_WINDEPLOYQT windeployqt HINTS "${QT6_INSTALL_PREFIX}" "${Qt6_DIR}/../../../bin"
                 DOC "Qt's Windows deployment tool, which stages the Qt runtime")
    if(ISOTONE_WINDEPLOYQT)
        # The CRT beside the app rather than windeployqt's vc_redist.x64.exe,
        # which is 18 MB of installer for 1.5 MB of DLLs. isotone.exe needs them
        # (it links Qt, which is /MD); IsoAPO.dll does not, because it links the
        # CRT statically for audiodg (core/CMakeLists.txt).
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${ISOTONE_INSTALL_BINDIR}")
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
        include(InstallRequiredSystemLibraries)

        install(CODE "
            set(exe \"\${CMAKE_INSTALL_PREFIX}/isotone.exe\")
            message(STATUS \"windeployqt \${exe}\")
            execute_process(
                COMMAND \"${ISOTONE_WINDEPLOYQT}\" --release --no-translations
                        --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime
                        --qmldir \"${CMAKE_CURRENT_SOURCE_DIR}/qml\" \"\${exe}\"
                RESULT_VARIABLE failed)
            if(failed)
                message(FATAL_ERROR \"windeployqt failed (\${failed}): the install has no Qt runtime\")
            endif()
            # Qt's runtime HLSL compiler, 15.5 MB, for shaders this app does not
            # have: nothing in qml/ is a ShaderEffect and nothing calls
            # qt_add_shaders, so these are only ever loaded to compile nothing.
            # windeployqt has no flag for them.
            foreach(unused dxcompiler.dll dxil.dll)
                file(REMOVE \"\${CMAKE_INSTALL_PREFIX}/\${unused}\")
            endforeach()
        " COMPONENT app)
    else()
        message(WARNING "windeployqt not found: `cmake --install` will lay out an app with no Qt runtime")
    endif()
else()
    # The desktop's half of an installed app: the launcher, the icon theme and
    # the AppStream metadata GNOME Software and KDE Discover read.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../linux/packaging/io.github.jackwangxyw.Isotone.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT app)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../linux/packaging/io.github.jackwangxyw.Isotone.metainfo.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo" COMPONENT app)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../linux/packaging/icons/"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons" COMPONENT app)
endif()

if(ISOTONE_BUILD_TESTS)
    # The backend without Qt. The Windows half drives IsoAPO's mapping, the
    # compat writer and the registry; the Linux half the daemon's region, XDG
    # autostart and a running PipeWire (skipped where there is none).
    if(WIN32)
        set(ISOTONE_UI_TESTS_PLATFORM tests/test_backend.cpp tests/test_config_attach.cpp
                                      tests/test_settings_backend.cpp tests/test_compat_retry.cpp)
    else()
        set(ISOTONE_UI_TESTS_PLATFORM tests/test_devicelink_posix.cpp tests/test_autostart_xdg.cpp
                                      tests/test_pipewire_outputs.cpp)
    endif()
    add_executable(ui_tests tests/test_main.cpp tests/test_typed_value.cpp tests/test_speaker_setup.cpp
                   tests/test_sine_tone.cpp ${ISOTONE_UI_TESTS_PLATFORM})
    target_link_libraries(ui_tests PRIVATE isotone_ui_backend)
    target_include_directories(ui_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests/third_party)
    if(MSVC)
        target_compile_options(ui_tests PRIVATE /wd5285)   # doctest, as in core/tests
    endif()
    add_test(NAME ui_tests COMMAND ui_tests)

    # Devices is Windows' devicetool and Equalizer APO; the rest is the model on both.
    if(WIN32)
        set(ISOTONE_UI_MODEL_TESTS_PLATFORM tests/test_devices.cpp)
    endif()
    add_executable(ui_model_tests tests/test_model.cpp tests/test_speakers_model.cpp tests/test_undo.cpp tests/test_presets.cpp
                   tests/test_settings.cpp tests/test_session_fixes.cpp tests/test_spectrum_display.cpp
                   tests/test_eqbyear.cpp tests/test_logomark.cpp tests/test_rig.h ${ISOTONE_UI_MODEL_TESTS_PLATFORM})
    target_link_libraries(ui_model_tests PRIVATE isotone_ui Qt6::Test)
    target_compile_definitions(ui_model_tests PRIVATE ISOTONE_UI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/devices")
    target_include_directories(ui_model_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests/third_party)
    if(MSVC)
        target_compile_options(ui_model_tests PRIVATE /wd5285)   # doctest, as in core/tests
    endif()
    add_test(NAME ui_model_tests COMMAND ui_model_tests)
    # Linux: the global hotkeys' X11 grabs need an X server; the suite above runs
    # offscreen, where that case skips itself.
    if(NOT WIN32)
        find_program(XVFB_RUN xvfb-run)
        if(XVFB_RUN)
            add_test(NAME ui_model_tests_x11
                     COMMAND ${XVFB_RUN} -a env QT_QPA_PLATFORM=xcb $<TARGET_FILE:ui_model_tests>
                             "-tc=global hotkeys grab on X11*")
        endif()
        # And the GlobalShortcuts portal's: a mock portal in a process of its own,
        # on a session bus of their own, where python3 has the D-Bus bindings.
        find_program(DBUS_RUN_SESSION dbus-run-session)
        find_package(Python3 COMPONENTS Interpreter)
        if(DBUS_RUN_SESSION AND Python3_Interpreter_FOUND)
            execute_process(COMMAND ${Python3_EXECUTABLE} -c "import dbus, gi" RESULT_VARIABLE ISOTONE_PY_DBUS
                            OUTPUT_QUIET ERROR_QUIET)
            if(ISOTONE_PY_DBUS EQUAL 0)
                add_test(NAME ui_model_tests_portal
                         COMMAND ${DBUS_RUN_SESSION} -- bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_portal_test.sh
                                 $<TARGET_FILE:ui_model_tests>)
            endif()
        endif()
    endif()

    # QML tests: Qt Quick Test cases in tests/qml driving the real components.
    qt_add_executable(ui_qml_tests tests/qml_main.cpp)
    target_link_libraries(ui_qml_tests PRIVATE isotone_ui isotone_uiplugin Qt6::QuickTest)
    target_compile_definitions(ui_qml_tests PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml")
    # Devices work package: the outputs and devicetool the QML tests see.
    target_compile_definitions(ui_qml_tests PRIVATE ISOTONE_FAKE_DEVICETOOL_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tests/devices/fake-devicetool.json")
    add_test(NAME ui_qml_tests COMMAND ui_qml_tests)
endif()
