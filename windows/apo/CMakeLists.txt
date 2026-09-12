# IsoAPO: the Windows audio processing object. x64 only; audiodg is 64-bit on
# 64-bit Windows, so the 32-bit build upstream carries is dropped (plan 3.1).
add_library(IsoAPO SHARED
    dllmain.cpp
    isoapo.cpp
    IsoAPO.def
)

target_link_libraries(IsoAPO PRIVATE
    isotone_core
    audiobaseprocessingobject
    audiomediatypecrt        # CreateAudioMediaTypeFromUncompressedAudioFormat
    legacy_stdio_definitions # audiobaseprocessingobject.lib wants _vsnwprintf
    ole32
    oleaut32
    uuid
    shell32
    advapi32
)

set_target_properties(IsoAPO PROPERTIES OUTPUT_NAME "IsoAPO" PREFIX "")

# CRegAPOProperties<1> in the Windows SDK declares a trailing zero-sized array,
# which MSVC warns about (C4815). SDK header, nothing for us to fix.
if(MSVC)
    target_compile_options(IsoAPO PRIVATE /wd4815)
    # audiomediatypecrt.lib carries /DEFAULTLIB:atls.lib, but the ATL static
    # library is not part of a default Build Tools install and none of its
    # symbols are actually referenced from the paths we use. Excluding it keeps
    # the build working without adding an ATL install to the prerequisites. If a
    # future change does pull in an ATL symbol, this will fail loudly at link
    # time rather than silently, which is the behaviour we want.
    target_link_options(IsoAPO PRIVATE /NODEFAULTLIB:atls.lib)
endif()

# Upstream links the CRT statically, which is the safer choice for a DLL loaded
# into audiodg.exe. Deferred: it has to apply to isotone_core as well or the two
# disagree about which CRT owns lround and copysign, so it is a packaging-time
# decision (stage 6) rather than something to half-apply here. A real Equalizer
# APO install already ships msvcp140.dll alongside its DLL, so the dynamic CRT
# demonstrably works in that process.

# Offline host for the APO: loads the DLL, drives the COM lifecycle and measures
# the audio, without registering anything or going near audiodg.
add_executable(isotone-apo-selftest apo_selftest.cpp)
target_link_libraries(isotone-apo-selftest PRIVATE
    audiomediatypecrt
    legacy_stdio_definitions
    ole32
    oleaut32
    uuid
)
add_dependencies(isotone-apo-selftest IsoAPO)
if(MSVC)
    target_link_options(isotone-apo-selftest PRIVATE /NODEFAULTLIB:atls.lib)
endif()
