# IsoAPO: the Windows audio processing object. x64 only; audiodg is 64-bit on
# 64-bit Windows, so the 32-bit build upstream carries is dropped (plan 3.1).
function(isoapo_dll target)
    add_library(${target} SHARED
        dllmain.cpp
        isoapo.cpp
        IsoAPO.def
    )

    target_link_libraries(${target} PRIVATE
        isotone_core
        isotone_transport
        audiobaseprocessingobject
        audiomediatypecrt        # CreateAudioMediaTypeFromUncompressedAudioFormat
        legacy_stdio_definitions # audiobaseprocessingobject.lib wants _vsnwprintf
        ole32
        oleaut32
        uuid
        shell32
        advapi32
    )

    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${target}" PREFIX "")

    # CRegAPOProperties<1> in the Windows SDK declares a trailing zero-sized array,
    # which MSVC warns about (C4815). SDK header, nothing for us to fix.
    if(MSVC)
        target_compile_options(${target} PRIVATE /wd4815)
        # audiomediatypecrt.lib carries /DEFAULTLIB:atls.lib, but the ATL static
        # library is not part of a default Build Tools install and none of its
        # symbols are actually referenced from the paths we use. Excluding it keeps
        # the build working without adding an ATL install to the prerequisites. If a
        # future change does pull in an ATL symbol, this will fail loudly at link
        # time rather than silently, which is the behaviour we want.
        target_link_options(${target} PRIVATE /NODEFAULTLIB:atls.lib)
    endif()
endfunction()

isoapo_dll(IsoAPO)

# The same sources with the shared region in the Local\ namespace instead of
# Global\. Creating a Global\ object needs SeCreateGlobalPrivilege, which audiodg
# has and an unelevated test process does not. This is the only difference, and
# this DLL must never be installed: the UI could not reach its region.
isoapo_dll(IsoAPO-selftest)
target_compile_definitions(IsoAPO-selftest PRIVATE ISOAPO_SELFTEST_LOCAL_NAMESPACE)

# Upstream links the CRT statically, which is the safer choice for a DLL loaded
# into audiodg.exe. Deferred: it has to apply to isotone_core as well or the two
# disagree about which CRT owns lround and copysign, so it is a packaging-time
# decision (stage 6) rather than something to half-apply here. A real Equalizer
# APO install already ships msvcp140.dll alongside its DLL, so the dynamic CRT
# demonstrably works in that process.

# Offline host for the APO: loads the DLL, drives the COM lifecycle, opens the
# shared region as the UI would, and measures the audio, without registering
# anything or going near audiodg.
add_executable(isotone-apo-selftest apo_selftest.cpp)
target_link_libraries(isotone-apo-selftest PRIVATE
    isotone_core
    isotone_transport
    audiomediatypecrt
    legacy_stdio_definitions
    ole32
    oleaut32
    uuid
    advapi32
)
add_dependencies(isotone-apo-selftest IsoAPO-selftest)
if(MSVC)
    target_link_options(isotone-apo-selftest PRIVATE /NODEFAULTLIB:atls.lib)
endif()
