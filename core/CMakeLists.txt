cmake_minimum_required(VERSION 3.24)
project(SonalisCore LANGUAGES CXX)

include(FetchContent)
if(NOT TARGET monocypher_static)
  FetchContent_Declare(monocypher
    URL https://monocypher.org/download/monocypher-4.0.2.tar.gz
    URL_HASH SHA256=38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(monocypher)
  add_library(monocypher_static STATIC
    ${monocypher_SOURCE_DIR}/src/monocypher.c
    ${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c)
  target_include_directories(monocypher_static PUBLIC
    ${monocypher_SOURCE_DIR}/src
    ${monocypher_SOURCE_DIR}/src/optional)
  if(MSVC)
    target_compile_options(monocypher_static PRIVATE /W0)
  else()
    target_compile_options(monocypher_static PRIVATE -w)
  endif()
endif()

if(NOT TARGET opus)
  set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(OPUS_CUSTOM_MODES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(opus
    URL https://github.com/xiph/opus/archive/refs/tags/v1.5.2.tar.gz
    URL_HASH SHA256=9480e329e989f70d69886ded470c7f8cfe6c0667cc4196d4837ac9e668fb7404
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(opus)
endif()

add_library(sonalis_core STATIC
  src/c_api.cpp
  src/c_voice_codec.cpp
  src/client_state.cpp
  src/portable_crypto.cpp
  src/voice_codec.cpp
  src/event_generation.cpp
  src/voice_session.cpp
)
target_compile_features(sonalis_core PUBLIC cxx_std_20)
target_include_directories(sonalis_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sonalis_core PUBLIC monocypher_static opus)

include(CTest)
if(BUILD_TESTING)
  add_executable(sonalis_core_tests tests/voice_session_test.cpp)
  target_link_libraries(sonalis_core_tests PRIVATE sonalis_core)
  add_test(NAME sonalis_core_voice_session COMMAND sonalis_core_tests)

  add_executable(sonalis_core_event_tests tests/event_generation_test.cpp)
  target_link_libraries(sonalis_core_event_tests PRIVATE sonalis_core)
  add_test(NAME sonalis_core_event_generation COMMAND sonalis_core_event_tests)

  add_executable(sonalis_core_c_api_tests tests/c_api_test.cpp)
  target_link_libraries(sonalis_core_c_api_tests PRIVATE sonalis_core)
  add_test(NAME sonalis_core_c_api COMMAND sonalis_core_c_api_tests)

  add_executable(sonalis_core_client_state_tests tests/client_state_test.cpp)
  target_link_libraries(sonalis_core_client_state_tests PRIVATE sonalis_core)
  add_test(NAME sonalis_core_client_state COMMAND sonalis_core_client_state_tests)

  add_executable(sonalis_core_crypto_tests tests/portable_crypto_test.cpp)
  target_link_libraries(sonalis_core_crypto_tests PRIVATE sonalis_core)
  add_test(NAME sonalis_core_portable_crypto COMMAND sonalis_core_crypto_tests)

  add_executable(sonalis_core_voice_codec_tests tests/voice_codec_test.cpp)
  target_link_libraries(sonalis_core_voice_codec_tests PRIVATE sonalis_core)
  add_test(NAME sonalis_core_voice_codec COMMAND sonalis_core_voice_codec_tests)
endif()
