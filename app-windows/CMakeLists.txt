# Windows desktop app: a webview-based single self-contained executable,
# mirroring app-linux/CMakeLists.txt (FetchContent the webview library and
# embed ui/index.html into the binary at build time via configure_file, so
# there are no runtime assets to locate) but targeting the Win32/WebView2
# backend via MSYS2 UCRT64 (MinGW-w64) instead of GTK3/WebKitGTK.

include(FetchContent)

# webview's own CMake defaults WEBVIEW_USE_COMPAT_MINGW to its
# "is this the top-level CMake project" check, which is false once
# FetchContent'd as a subdirectory here -- force it on so MinGW gets the
# WebView2 COM interface shims it needs (MinGW's own headers lag MSVC's).
set(WEBVIEW_USE_COMPAT_MINGW ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  webview
  GIT_REPOSITORY https://github.com/webview/webview
  GIT_TAG 0.12.0)
FetchContent_MakeAvailable(webview)

# ui/ and assets/ live at the repo root, shared with app-linux.
set(REMBOARD_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ui")
set(REMBOARD_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../assets")

# index.html is the template; style.css/app.js/qrcode.min.js are kept as
# separate files for readability and get inlined into it here, so the
# shipped binary still ends up with a single self-contained page.
file(READ "${REMBOARD_UI_DIR}/index.html" INDEX_HTML_CONTENT)
file(READ "${REMBOARD_UI_DIR}/style.css" STYLE_CSS_CONTENT)
file(READ "${REMBOARD_UI_DIR}/app.js" APP_JS_CONTENT)
file(READ "${REMBOARD_UI_DIR}/qrcode.min.js" QRCODE_JS_CONTENT)
string(REPLACE "/*__STYLE_CSS__*/" "${STYLE_CSS_CONTENT}" INDEX_HTML_CONTENT "${INDEX_HTML_CONTENT}")
string(REPLACE "/*__QRCODE_JS__*/" "${QRCODE_JS_CONTENT}" INDEX_HTML_CONTENT "${INDEX_HTML_CONTENT}")
string(REPLACE "/*__APP_JS__*/" "${APP_JS_CONTENT}" INDEX_HTML_CONTENT "${INDEX_HTML_CONTENT}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${REMBOARD_UI_DIR}/index.html"
  "${REMBOARD_UI_DIR}/app.js"
  "${REMBOARD_UI_DIR}/style.css"
  "${REMBOARD_UI_DIR}/qrcode.min.js"
  "${REMBOARD_ASSETS_DIR}/icon.png")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/generated_html.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/generated_html.h"
  @ONLY)

# Same "no runtime assets" convention as the HTML above: the window icon is
# baked into the binary as a byte array rather than shipped/looked up as a
# separate file.
file(READ "${REMBOARD_ASSETS_DIR}/icon.png" REMBOARD_ICON_PNG_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," REMBOARD_ICON_PNG_ARRAY "${REMBOARD_ICON_PNG_HEX}")
string(LENGTH "${REMBOARD_ICON_PNG_HEX}" _icon_hex_len)
math(EXPR REMBOARD_ICON_PNG_LEN "${_icon_hex_len} / 2")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/generated_icon.h"
"#pragma once
#include <cstddef>
inline constexpr unsigned char kIconPng[] = {${REMBOARD_ICON_PNG_ARRAY}};
inline constexpr std::size_t kIconPngLen = ${REMBOARD_ICON_PNG_LEN};
")

# WIN32 (no console window); fatal startup errors are reported via
# MessageBox instead of stderr since there's no console to see it on.
add_executable(remboard WIN32
  src/main.cpp
  src/bridge.cpp
  src/file_secret_store.cpp)

target_include_directories(remboard PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(remboard PRIVATE
  webview::core
  remboard_core
  gdiplus)

target_compile_features(remboard PRIVATE cxx_std_23)

# Portable-folder distribution (see plan): statically link the MinGW C/C++
# runtime so libgcc/libstdc++ DLLs don't need to be shipped; the remaining
# MSYS2-packaged shared libs (zeromq/sodium/protobuf/abseil/winpthread) do
# still need to sit next to remboard.exe -- check with `ldd` after building.
target_link_options(remboard PRIVATE -static-libgcc -static-libstdc++)
