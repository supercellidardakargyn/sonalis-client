cmake_minimum_required(VERSION 3.24)
project(SonalisAndroidCore LANGUAGES CXX)

set(SONALIS_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../..")
add_subdirectory("${SONALIS_CLIENT_ROOT}/core" "${CMAKE_CURRENT_BINARY_DIR}/sonalis-core")

add_library(sonalis_android SHARED
  native_audio_engine.cpp
  native_crypto.cpp
  native_voice_codec.cpp
  native_voice_session.cpp
)
target_compile_features(sonalis_android PRIVATE cxx_std_20)
target_link_libraries(sonalis_android PRIVATE sonalis_core aaudio log)
target_compile_options(sonalis_android PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
