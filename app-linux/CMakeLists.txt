# Linux desktop app: a webview-based single self-contained executable,
# following the same convention used by this developer's other webview
# apps (see ~/dev/apps/sysview) -- FetchContent the webview library and
# embed ui/index.html into the binary at build time via configure_file, so
# there are no runtime assets to locate.

include(FetchContent)
FetchContent_Declare(
  webview
  GIT_REPOSITORY https://github.com/webview/webview
  GIT_TAG 0.12.0)
FetchContent_MakeAvailable(webview)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

# index.html is the template; style.css/app.js/qrcode.min.js are kept as
# separate files for readability and get inlined into it here, so the
# shipped binary still ends up with a single self-contained page.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html" INDEX_HTML_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ui/style.css" STYLE_CSS_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ui/app.js" APP_JS_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ui/qrcode.min.js" QRCODE_JS_CONTENT)
string(REPLACE "/*__STYLE_CSS__*/" "${STYLE_CSS_CONTENT}" INDEX_HTML_CONTENT "${INDEX_HTML_CONTENT}")
string(REPLACE "/*__QRCODE_JS__*/" "${QRCODE_JS_CONTENT}" INDEX_HTML_CONTENT "${INDEX_HTML_CONTENT}")
string(REPLACE "/*__APP_JS__*/" "${APP_JS_CONTENT}" INDEX_HTML_CONTENT "${INDEX_HTML_CONTENT}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html"
  "${CMAKE_CURRENT_SOURCE_DIR}/ui/app.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/ui/style.css"
  "${CMAKE_CURRENT_SOURCE_DIR}/ui/qrcode.min.js")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/generated_html.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/generated_html.h"
  @ONLY)

add_executable(remboard
  src/main.cpp
  src/bridge.cpp
  src/file_secret_store.cpp)

target_include_directories(remboard PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(remboard PRIVATE
  webview::core
  remboard_core
  PkgConfig::GTK3)

target_compile_features(remboard PRIVATE cxx_std_23)
