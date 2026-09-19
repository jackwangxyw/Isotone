# isotone-devicetool: IsoAPO device registration (plan 5.2), wrapping upstream
# Equalizer APO's DeviceAPOInfo and RegistryHelper, vendored under upstream/.
add_executable(isotone-devicetool
    main.cpp
    dry_run.cpp
    upstream/AbstractAPOInfo.cpp
    upstream/DeviceAPOInfo.cpp
    upstream/helpers/RegistryHelper.cpp
    upstream/helpers/ServiceHelper.cpp
    upstream/helpers/StringHelper.cpp
)

# upstream/ first, so the vendored files find their own stdafx.h and helpers/.
target_include_directories(isotone-devicetool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(isotone-devicetool PRIVATE UNICODE _UNICODE)

# DeviceAPOInfo::load's parameter shadows a member (C4458). Vendored code, left
# as upstream wrote it.
set_source_files_properties(upstream/DeviceAPOInfo.cpp PROPERTIES COMPILE_OPTIONS /wd4458)
# isotone_devices for layouts and set-layout.
target_link_libraries(isotone-devicetool PRIVATE
    isotone_devices ole32 oleaut32 uuid advapi32 shell32 shlwapi version authz userenv
)

# The UI's side of `serve`: an elevated devicetool for the life of the app.
isotone_version_resource(isotone-devicetool "Isotone device tool")

add_library(isotone_devicetool_session STATIC session.cpp)
target_include_directories(isotone_devicetool_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(isotone_devicetool_session PRIVATE UNICODE _UNICODE)
target_link_libraries(isotone_devicetool_session PRIVATE ole32 advapi32 shell32)
