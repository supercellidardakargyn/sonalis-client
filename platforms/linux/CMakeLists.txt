cmake_minimum_required(VERSION 3.24)
project(SonalisLinux LANGUAGES C CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
add_subdirectory(../../core ${CMAKE_CURRENT_BINARY_DIR}/sonalis-core)

add_executable(sonalis-linux
  main.cpp
  src/curl_http_client.cpp
  src/linux_api.cpp
  src/linux_voice_call.cpp
  src/linux_voice_transport.cpp
  src/pipewire_audio_backend.cpp
  src/secret_service_store.cpp
)
target_compile_features(sonalis-linux PRIVATE cxx_std_20)
target_compile_options(sonalis-linux PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(sonalis-linux PRIVATE include)
target_link_libraries(sonalis-linux PRIVATE
  sonalis_core
  PkgConfig::GTK4
  PkgConfig::LIBSECRET
  PkgConfig::PIPEWIRE
  PkgConfig::JSON_GLIB
  CURL::libcurl
  OpenSSL::SSL
  OpenSSL::Crypto
)
install(TARGETS sonalis-linux RUNTIME DESTINATION bin)
install(FILES sonalis.desktop DESTINATION share/applications)
install(FILES ../../logo.png
  DESTINATION share/icons/hicolor/256x256/apps
  RENAME sonalis.png)
install(FILES tr.sonalis.Sonalis.metainfo.xml
  DESTINATION share/metainfo)

set(CPACK_PACKAGE_NAME "sonalis")
set(CPACK_PACKAGE_VENDOR "Sonalis Iletisim Teknolojileri A.S.")
set(CPACK_PACKAGE_CONTACT "support@sonalis.tr")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Dusuk gecikmeli guvenli ses ve topluluk istemcisi")
set(CPACK_PACKAGE_VERSION "5.2.0" CACHE STRING "Sonalis package version")
set(CPACK_PACKAGE_FILE_NAME "Sonalis-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSES.md")
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS
  "libgtk-4-1, libsecret-1-0, libpipewire-0.3-0, libjson-glib-1.0-0, libcurl4, libssl3")
if(EXISTS "/usr/bin/rpmbuild")
  list(APPEND CPACK_GENERATOR "RPM")
  set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
  set(CPACK_RPM_PACKAGE_LICENSE "Proprietary")
  set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
  set(CPACK_RPM_PACKAGE_REQUIRES
    "gtk4, libsecret, pipewire-libs, json-glib, libcurl, openssl-libs")
endif()
include(CPack)
