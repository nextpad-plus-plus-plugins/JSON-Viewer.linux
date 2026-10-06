cmake_minimum_required(VERSION 3.20)
# Keep PROJECT_VERSION in sync with the macOS port's plugin version.
project(NppJsonViewer VERSION 1.0.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Host plugin interface + Scintilla (read-only; NO host changes)
set(NPP_LINUX_DIR      "${CMAKE_SOURCE_DIR}/../../nextpad-plus-plus-gtk4")
set(NPP_PLUGIN_INCLUDE "${NPP_LINUX_DIR}/src")
set(SCINTILLA_INCLUDE  "${NPP_LINUX_DIR}/scintilla/include")

# Bundled header-only RapidJSON (NPP-JSONViewer fork — RawNumber fix +
# the SetLineEnding PrettyWriter extension; NOT interchangeable with stock).
set(RAPIDJSON_INCLUDE "${CMAKE_SOURCE_DIR}/external/rapidjson_include")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk4 glib-2.0 gio-2.0)

add_library(NppJsonViewer SHARED
  src/NppJsonViewer.cpp
  src/JsonParser.cpp
  src/JsonFormatter.cpp
  src/JsonPanel.cpp
  src/JsonSettingsDialog.cpp
  src/LinuxViewBridge.cpp)
target_include_directories(NppJsonViewer PRIVATE
    ${NPP_PLUGIN_INCLUDE} ${SCINTILLA_INCLUDE} ${RAPIDJSON_INCLUDE}
    "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(NppJsonViewer PRIVATE PkgConfig::GTK ${CMAKE_DL_LIBS})
target_compile_options(NppJsonViewer PRIVATE
    -Wall -Wno-unused-parameter -fvisibility=hidden -fPIC)
# scintilla_view_send_message resolves at dlopen from the host's libscintilla.so.
target_link_options(NppJsonViewer PRIVATE -Wl,--allow-shlib-undefined)
set_target_properties(NppJsonViewer PROPERTIES PREFIX "" OUTPUT_NAME "NppJsonViewer")

# ---- tests ------------------------------------------------------------------
# loader: dlopen smoke test (getName + menu + FuncItem ABI), no host needed.
add_executable(loader test/loader.cpp)
target_include_directories(loader PRIVATE
    "${CMAKE_SOURCE_DIR}/src" ${NPP_PLUGIN_INCLUDE} ${SCINTILLA_INCLUDE})
target_link_libraries(loader PRIVATE PkgConfig::GTK ${CMAKE_DL_LIBS})

# jv_test: drives the REAL plugin .so through the REAL bridge against a mock
# Scintilla document. ENABLE_EXPORTS puts this executable's dynamic symbols in
# the global scope so the plugin's undefined scintilla_view_send_message binds
# to the mock here (the same way it binds to the host's libscintilla.so).
add_executable(jv_test test/jv_test.cpp)
target_include_directories(jv_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src" ${NPP_PLUGIN_INCLUDE} ${SCINTILLA_INCLUDE}
    ${RAPIDJSON_INCLUDE})
target_link_libraries(jv_test PRIVATE PkgConfig::GTK ${CMAKE_DL_LIBS})
set_target_properties(jv_test PROPERTIES ENABLE_EXPORTS ON)

enable_testing()
add_test(NAME plugin_loader COMMAND loader $<TARGET_FILE:NppJsonViewer>)
# Needs a display (the bridge walks a real GTK widget tree) — run under xvfb-run.
add_test(NAME jv_behaviour COMMAND jv_test $<TARGET_FILE:NppJsonViewer>)

# ---- install ----------------------------------------------------------------
set(PLUGIN_INSTALL_DIR "$ENV{HOME}/.local/share/nextpad++/plugins/NppJsonViewer"
    CACHE PATH "Where to install the plugin folder")
install(TARGETS NppJsonViewer DESTINATION "${PLUGIN_INSTALL_DIR}")
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION "${PLUGIN_INSTALL_DIR}")
install(DIRECTORY resources/ DESTINATION "${PLUGIN_INSTALL_DIR}/resources"
    PATTERN ".DS_Store" EXCLUDE)

# ---- distributable zip ------------------------------------------------------
set(JV_STAGE "${CMAKE_BINARY_DIR}/pkg/NppJsonViewer")
file(MAKE_DIRECTORY "${JV_STAGE}")
add_custom_target(package_zip
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:NppJsonViewer> "${JV_STAGE}/NppJsonViewer.so"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/LICENSE" "${JV_STAGE}/"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/resources" "${JV_STAGE}/resources"
  COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/pkg"
          ${CMAKE_COMMAND} -E tar cf "${CMAKE_BINARY_DIR}/JSON-Viewer.linux.zip"
          --format=zip NppJsonViewer
  DEPENDS NppJsonViewer
  COMMENT "Packaging JSON-Viewer.linux.zip")
