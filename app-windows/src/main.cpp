#include <windows.h>

#include <objidl.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <webview/webview.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "bridge.h"
#include "file_secret_store.h"
#include "generated_html.h"
#include "generated_icon.h"
#include "remboard/core.h"
#include "remboard/platform/discovery_mdns.h"

namespace {

// Decodes the embedded PNG and sets it as the window/taskbar icon via GDI+
// (Windows has no PNG decoder in plain Win32). The webview library has no
// icon API of its own, so this reaches into the native HWND it created
// (window() returns it as void* on the Win32 backend) directly.
void set_window_icon(webview::webview& w) {
  auto window = w.window();
  if (!window.ok()) return;
  HWND hwnd = static_cast<HWND>(window.value());

  ULONG_PTR gdiplus_token = 0;
  Gdiplus::GdiplusStartupInput startup_input;
  if (Gdiplus::GdiplusStartup(&gdiplus_token, &startup_input, nullptr) != Gdiplus::Ok) return;

  if (IStream* stream = SHCreateMemStream(kIconPng, static_cast<UINT>(kIconPngLen))) {
    Gdiplus::Bitmap bitmap(stream);
    HICON icon = nullptr;
    if (bitmap.GetHICON(&icon) == Gdiplus::Ok) {
      SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
      SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }
    stream->Release();
  }
  Gdiplus::GdiplusShutdown(gdiplus_token);
}

std::string default_display_name() {
  char buf[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD size = sizeof(buf);
  if (GetComputerNameA(buf, &size)) return std::string(buf) + "'s PC";
  return "My PC";
}

// --port/--config-dir/--advertise-ip/--name let a second instance run
// alongside the default one on the same machine for local testing; none
// are needed for normal single-instance use.
std::optional<std::string> arg_value(const std::vector<std::string>& args,
                                      const std::string& flag) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == flag) return args[i + 1];
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);

  try {
    std::string config_dir =
        arg_value(args, "--config-dir").value_or(std::string());
    remboard_app::FileSecretStore secret_store =
        config_dir.empty() ? remboard_app::FileSecretStore()
                            : remboard_app::FileSecretStore(config_dir);
    auto discovery = remboard::make_mdns_discovery();

    remboard::PlatformHooks hooks;
    hooks.secret_store = &secret_store;
    hooks.discovery = discovery.get();
    hooks.data_dir = secret_store.config_dir();
    hooks.display_name =
        arg_value(args, "--name").value_or(default_display_name());
    hooks.platform = remboard::Platform::kWindows;
    if (auto port = arg_value(args, "--port"))
      hooks.listen_port = static_cast<uint16_t>(std::stoi(*port));
    hooks.advertise_ip_override =
        arg_value(args, "--advertise-ip").value_or(std::string());

    auto core = remboard::Core::create(hooks);

    webview::webview w(false, nullptr);
    w.set_title("remboard — " + hooks.display_name);
    w.set_size(920, 640, WEBVIEW_HINT_NONE);
    set_window_icon(w);

    remboard_app::Bridge bridge(*core, w);
    bridge.register_handlers();
    bridge.register_push_callbacks();

    core->start();

    w.set_html(kIndexHtml);
    w.run();

    core->shutdown();
  } catch (const webview::exception& e) {
    MessageBoxA(nullptr, e.what(), "remboard", MB_OK | MB_ICONERROR);
    return 1;
  } catch (const std::exception& e) {
    MessageBoxA(nullptr, e.what(), "remboard", MB_OK | MB_ICONERROR);
    return 1;
  }

  return 0;
}
