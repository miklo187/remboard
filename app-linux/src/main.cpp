#include <unistd.h>

#include <webview/webview.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "bridge.h"
#include "file_secret_store.h"
#include "generated_html.h"
#include "remboard/core.h"
#include "remboard/platform/discovery_avahi.h"

namespace {

std::string default_display_name() {
  if (const char* host = std::getenv("HOSTNAME");
      host != nullptr && host[0] != '\0')
    return std::string(host) + "'s PC";
  char buf[256];
  if (gethostname(buf, sizeof(buf)) == 0) return std::string(buf) + "'s PC";
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
    auto discovery = remboard::make_avahi_discovery();

    remboard::PlatformHooks hooks;
    hooks.secret_store = &secret_store;
    hooks.discovery = discovery.get();
    hooks.data_dir = secret_store.config_dir();
    hooks.display_name =
        arg_value(args, "--name").value_or(default_display_name());
    hooks.platform = remboard::Platform::kLinux;
    if (auto port = arg_value(args, "--port"))
      hooks.listen_port = static_cast<uint16_t>(std::stoi(*port));
    hooks.advertise_ip_override =
        arg_value(args, "--advertise-ip").value_or(std::string());

    auto core = remboard::Core::create(hooks);

    webview::webview w(false, nullptr);
    w.set_title("remboard — " + hooks.display_name);
    w.set_size(920, 640, WEBVIEW_HINT_NONE);

    remboard_app::Bridge bridge(*core, w);
    bridge.register_handlers();
    bridge.register_push_callbacks();

    core->start();

    w.set_html(kIndexHtml);
    w.run();

    core->shutdown();
  } catch (const webview::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "remboard: " << e.what() << '\n';
    return 1;
  }

  return 0;
}
