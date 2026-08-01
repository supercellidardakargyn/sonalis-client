cmake_minimum_required(VERSION 3.24)
project(Sonalis VERSION 5.2.0 LANGUAGES C CXX RC)

if(MSVC)
    # CMake's Windows defaults link Shell32 even when the application has no
    # eager Shell API calls. Rare shell operations are resolved dynamically.
    string(REPLACE "shell32.lib" "" CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES}")
    string(REPLACE "shell32.lib" "" CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
option(SONALIS_REQUIRE_AUTHENTICODE "Reject unsigned or modified Sonalis client binaries at startup" OFF)
option(SONALIS_BUILD_TESTS "Build local unit tests" OFF)
option(SONALIS_STAGE_VERIFIED_EXE "Copy a verified Release executable to client/Sonalis.exe" OFF)
set(SONALIS_AUTHENTICODE_CERT_SHA256 "" CACHE STRING "Pinned SHA-256 fingerprint of the production Authenticode signing certificate")
if(SONALIS_REQUIRE_AUTHENTICODE AND NOT SONALIS_AUTHENTICODE_CERT_SHA256 MATCHES "^[0-9A-Fa-f]{64}$")
    message(FATAL_ERROR "SONALIS_REQUIRE_AUTHENTICODE requires a 64-character SONALIS_AUTHENTICODE_CERT_SHA256")
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# The platform-independent lifecycle is part of every native client build. Its
# tests are also pulled into the normal Windows test configuration so a future
# platform cannot silently diverge from the P2P/sleep policy.
set(BUILD_TESTING ${SONALIS_BUILD_TESTS} CACHE BOOL "Build Sonalis tests" FORCE)
include(CTest)
add_subdirectory(core)

set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_CUSTOM_MODES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus
    URL https://github.com/xiph/opus/archive/refs/tags/v1.5.2.tar.gz
    URL_HASH SHA256=9480e329e989f70d69886ded470c7f8cfe6c0667cc4196d4837ac9e668fb7404
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(opus)

FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.tar.gz
    URL_HASH SHA256=fecb33d33930e12ff53a34064e9d3a06c8f7c3e04408f14cd36c80e3faac863b
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(monocypher
    URL https://monocypher.org/download/monocypher-4.0.2.tar.gz
    URL_HASH SHA256=38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(monocypher)
if(NOT TARGET monocypher_static)
    add_library(monocypher_static STATIC
        ${monocypher_SOURCE_DIR}/src/monocypher.c
        ${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c)
    target_include_directories(monocypher_static PUBLIC
        ${monocypher_SOURCE_DIR}/src
        ${monocypher_SOURCE_DIR}/src/optional)
    if(MSVC)
        target_compile_options(monocypher_static PRIVATE /W0)
    endif()
endif()

set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_SERVER ON CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libjuice
    GIT_REPOSITORY https://github.com/paullouisageneau/libjuice.git
    GIT_TAG v1.7.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(libjuice)
if(MSVC)
    target_compile_options(juice-static PRIVATE /W0)
endif()

FetchContent_Declare(rnnoise
    URL https://github.com/xiph/rnnoise/archive/refs/tags/v0.2.tar.gz
    URL_HASH SHA256=e4a17eb1493b7551f9f7781d2acc67b8847d6ed7c4b64db0fd62c017021ae1ec
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(rnnoise)
FetchContent_Declare(rnnoise_model
    URL https://media.xiph.org/rnnoise/models/rnnoise_data-0b50c45.tar.gz
    URL_HASH SHA256=4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(rnnoise_model)
add_library(rnnoise_static STATIC
    ${rnnoise_SOURCE_DIR}/src/denoise.c
    ${rnnoise_SOURCE_DIR}/src/rnn.c
    ${rnnoise_SOURCE_DIR}/src/pitch.c
    ${rnnoise_SOURCE_DIR}/src/kiss_fft.c
    ${rnnoise_SOURCE_DIR}/src/celt_lpc.c
    ${rnnoise_SOURCE_DIR}/src/nnet.c
    ${rnnoise_SOURCE_DIR}/src/nnet_default.c
    ${rnnoise_SOURCE_DIR}/src/x86/x86_dnn_map.c
    ${rnnoise_SOURCE_DIR}/src/x86/x86cpu.c
    ${rnnoise_SOURCE_DIR}/src/x86/nnet_sse4_1.c
    ${rnnoise_SOURCE_DIR}/src/x86/nnet_avx2.c
    ${rnnoise_SOURCE_DIR}/src/parse_lpcnet_weights.c
    ${rnnoise_model_SOURCE_DIR}/src/rnnoise_data.c
    ${rnnoise_SOURCE_DIR}/src/rnnoise_tables.c)
target_include_directories(rnnoise_static PUBLIC ${rnnoise_SOURCE_DIR}/include PRIVATE
    ${rnnoise_SOURCE_DIR}/src
    ${rnnoise_model_SOURCE_DIR}/src
    ${opus_SOURCE_DIR}/celt
    ${opus_SOURCE_DIR}/include)
target_compile_definitions(rnnoise_static PRIVATE
    RNNOISE_BUILD
    RNN_ENABLE_X86_RTCD
    OPUS_X86_MAY_HAVE_SSE2
    OPUS_X86_MAY_HAVE_SSE4_1
    OPUS_X86_MAY_HAVE_AVX2)
if(MSVC)
    target_compile_options(rnnoise_static PRIVATE /W0 /O2)
    set_source_files_properties(${rnnoise_SOURCE_DIR}/src/x86/nnet_avx2.c
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
endif()

set(SONALIS_RELEASE_PUBLIC_KEY_BASE64 "UQGB1ADGcLI8AmBCLxThYfwVaKTTpPpH70rYIt2DSIg=" CACHE STRING "Ed25519 release verification public key")
set(SONALIS_UPDATE_CHANNEL "stable" CACHE STRING "Update channel: stable or canary")
set_property(CACHE SONALIS_UPDATE_CHANNEL PROPERTY STRINGS stable canary)
if(NOT SONALIS_UPDATE_CHANNEL STREQUAL "stable" AND NOT SONALIS_UPDATE_CHANNEL STREQUAL "canary")
    message(FATAL_ERROR "SONALIS_UPDATE_CHANNEL must be stable or canary")
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})
target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_DEMO_WINDOWS
    IMGUI_DISABLE_DEBUG_TOOLS
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_DISABLE_FILE_FUNCTIONS
    IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
    WIN32_LEAN_AND_MEAN
    NOMINMAX)

if(MSVC)
    target_compile_options(imgui PRIVATE /W0)
endif()

add_executable(Sonalis WIN32
    main.cpp
    application_messages.h
    audio_engine.cpp
    audio_engine.h
    network.cpp
    network.h
    ice_transport.cpp
    ice_transport.h
    opus_codec.cpp
    opus_codec.h
    protocol.cpp
    protocol.h
    http_client.cpp
    http_client.h
    credential_vault.cpp
    credential_vault.h
    device_identity.cpp
    device_identity.h
    diagnostics.cpp
    diagnostics.h
    performance.cpp
    performance.h
    background_worker.cpp
    background_worker.h
    realtime_client.cpp
    realtime_client.h
    message_crypto.cpp
    media_crypto.cpp
    guardian_local_scanner.cpp
    guardian_local_scanner.h
    message_crypto.h
    native_device_signer.cpp
    native_device_signer.h
    platform_api.cpp
    platform_api.h
    json_contract.h
    update_service.cpp
    update_service.h
    noise_suppressor.cpp
    noise_suppressor.h
    settings.cpp
    settings.h
    theme_engine.cpp
    theme_engine.h
    horizon_layout.cpp
    horizon_layout.h
    horizon_icons.cpp
    horizon_icons.h
    localization.cpp
    localization.h
    win_helpers.h
    ui.cpp
    ui.h
    resources.rc)

target_compile_definitions(Sonalis PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    $<$<BOOL:${SONALIS_REQUIRE_AUTHENTICODE}>:SONALIS_REQUIRE_AUTHENTICODE=1>
    $<$<BOOL:${SONALIS_REQUIRE_AUTHENTICODE}>:SONALIS_AUTHENTICODE_CERT_SHA256="${SONALIS_AUTHENTICODE_CERT_SHA256}">)
target_compile_options(Sonalis PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /EHsc /utf-8 /GS /guard:cf /sdl>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/GL>)
target_link_options(Sonalis PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/OPT:REF /OPT:ICF /guard:cf /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /NODEFAULTLIB:shell32.lib>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/LTCG>)
target_link_libraries(Sonalis PRIVATE
    sonalis_core
    imgui
    opus
    nlohmann_json::nlohmann_json
    monocypher_static
    juice-static
    rnnoise_static
    d3d11
    dwmapi
    dxgi
    ws2_32
    ole32
    avrt
    propsys
    psapi
    uuid)
target_link_libraries(Sonalis PRIVATE winhttp bcrypt ncrypt advapi32 crypt32 wintrust comdlg32)
target_compile_definitions(Sonalis PRIVATE
    SONALIS_VERSION="${PROJECT_VERSION}"
    SONALIS_UPDATE_CHANNEL="${SONALIS_UPDATE_CHANNEL}"
    SONALIS_RELEASE_PUBLIC_KEY_BASE64="${SONALIS_RELEASE_PUBLIC_KEY_BASE64}")

# Guardian runs as a short-lived, sandboxable sidecar so loading an ONNX model
# does not permanently increase the native voice client's working set. WinML is
# part of supported Windows 10/11 installations and can select CPU or DirectML.
add_executable(SonalisGuardianScanner guardian_scanner.cpp)
target_compile_definitions(SonalisGuardianScanner PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(SonalisGuardianScanner PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /EHsc /utf-8 /GS /guard:cf /sdl>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/GL>)
target_link_options(SonalisGuardianScanner PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/OPT:REF /OPT:ICF /guard:cf /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/LTCG>)
target_link_libraries(SonalisGuardianScanner PRIVATE windowsapp runtimeobject)

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sonalis-build-$<CONFIG>.json" CONTENT
    "{\n  \"product\": \"client\",\n  \"version\": \"${PROJECT_VERSION}\",\n  \"channel\": \"${SONALIS_UPDATE_CHANNEL}\",\n  \"configuration\": \"$<CONFIG>\",\n  \"authenticodeRequired\": $<IF:$<BOOL:${SONALIS_REQUIRE_AUTHENTICODE}>,true,false>\n}\n")

if(SONALIS_STAGE_VERIFIED_EXE)
    add_custom_target(stage_client_executable
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/stage-verified-client.ps1"
            -Executable "$<TARGET_FILE:Sonalis>"
            -ScannerExecutable "$<TARGET_FILE:SonalisGuardianScanner>"
            -BuildMetadata "${CMAKE_CURRENT_BINARY_DIR}/sonalis-build-$<CONFIG>.json"
            -Destination "${CMAKE_CURRENT_SOURCE_DIR}/Sonalis.exe"
            -ScannerDestination "${CMAKE_CURRENT_SOURCE_DIR}/SonalisGuardianScanner.exe"
        DEPENDS Sonalis SonalisGuardianScanner
        VERBATIM
        COMMENT "Verifying and staging the x64 Release executable")
endif()

if(SONALIS_BUILD_TESTS)
    add_executable(SonalisTests client_tests.cpp protocol.cpp opus_codec.cpp device_identity.cpp localization.cpp
        horizon_layout.cpp theme_engine.cpp performance.cpp)
    target_compile_definitions(SonalisTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(SonalisTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /EHsc /utf-8>)
    target_link_libraries(SonalisTests PRIVATE opus bcrypt advapi32 psapi imgui nlohmann_json::nlohmann_json)
    add_test(NAME sonalis_windows_client COMMAND SonalisTests)
endif()
