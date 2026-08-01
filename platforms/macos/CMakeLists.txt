cmake_minimum_required(VERSION 3.24)
project(SonalisMac LANGUAGES C CXX OBJCXX)

add_subdirectory(../../core ${CMAKE_CURRENT_BINARY_DIR}/sonalis-core)
add_executable(SonalisMac MACOSX_BUNDLE main.mm)
target_compile_features(SonalisMac PRIVATE cxx_std_20)
target_compile_options(SonalisMac PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(SonalisMac PRIVATE sonalis_core
  "-framework AppKit" "-framework AVFoundation" "-framework Network"
  "-framework Security" "-framework UserNotifications")
set_target_properties(SonalisMac PROPERTIES
  MACOSX_BUNDLE_GUI_IDENTIFIER tr.sonalis.desktop
  MACOSX_BUNDLE_BUNDLE_NAME Sonalis
  MACOSX_BUNDLE_SHORT_VERSION_STRING 5.1.0
  MACOSX_BUNDLE_BUNDLE_VERSION 1)
