// Phase A verification tool: drives remboard_core directly (no UI) so the
// transport/pairing/chunking logic can be proven end-to-end before any app
// exists. Run two instances (different --data-dir / --port) and feed them
// newline commands on stdin; see docs/PAIRING.md or the plan for the
// verification script.

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "remboard/core.h"
#include "remboard/isecret_store.h"

namespace {

// Minimal flat-file ISecretStore for the harness: <data_dir>/<key>.json,
// mode 0600. app-linux has its own, more complete implementation.
class FileSecretStore : public remboard::ISecretStore {
 public:
  explicit FileSecretStore(std::string data_dir)
      : data_dir_(std::move(data_dir)) {
    std::filesystem::create_directories(data_dir_);
  }

  std::optional<std::string> load(const std::string& key) override {
    std::ifstream in(path_for(key), std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  void save(const std::string& key, const std::string& value) override {
    std::string path = path_for(key);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << value;
    out.close();
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
  }

 private:
  std::string path_for(const std::string& key) const {
    return data_dir_ + "/" + key + ".json";
  }
  std::string data_dir_;
};

std::vector<std::string> split(const std::string& line) {
  std::istringstream iss(line);
  std::vector<std::string> parts;
  std::string tok;
  while (iss >> tok) parts.push_back(tok);
  return parts;
}

std::string item_kind_name(remboard::ItemKind k) {
  switch (k) {
    case remboard::ItemKind::kText:
      return "text";
    case remboard::ItemKind::kClipboardText:
      return "clipboard";
    case remboard::ItemKind::kFile:
      return "file";
  }
  return "?";
}

void print_usage() {
  std::cerr << "usage: cli_harness identity --data-dir <dir> [--name <n>]\n"
            << "       cli_harness serve --data-dir <dir> --port <p> "
               "[--name <n>] [--advertise-ip <ip>]\n";
}

std::optional<std::string> arg_value(const std::vector<std::string>& args,
                                      const std::string& flag) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == flag) return args[i + 1];
  }
  return std::nullopt;
}

int run_identity(const std::vector<std::string>& args) {
  auto data_dir = arg_value(args, "--data-dir");
  if (!data_dir) {
    print_usage();
    return 1;
  }
  std::string name = arg_value(args, "--name").value_or("remboard-device");

  FileSecretStore store(*data_dir);
  remboard::PlatformHooks hooks;
  hooks.secret_store = &store;
  hooks.data_dir = *data_dir;
  hooks.display_name = name;
  hooks.platform = remboard::Platform::kLinux;

  auto core = remboard::Core::create(hooks);
  auto self = core->self_info();
  std::cout << "device_uuid=" << self.device_uuid << "\n";
  std::cout << "display_name=" << self.display_name << "\n";
  return 0;
}

int run_serve(const std::vector<std::string>& args) {
  auto data_dir = arg_value(args, "--data-dir");
  auto port_str = arg_value(args, "--port");
  if (!data_dir || !port_str) {
    print_usage();
    return 1;
  }
  std::string name = arg_value(args, "--name").value_or("remboard-device");
  std::string advertise_ip = arg_value(args, "--advertise-ip").value_or("");

  static FileSecretStore store(*data_dir);
  remboard::PlatformHooks hooks;
  hooks.secret_store = &store;
  hooks.discovery = nullptr;  // mDNS not exercised by the CLI harness
  hooks.data_dir = *data_dir;
  hooks.display_name = name;
  hooks.platform = remboard::Platform::kLinux;
  hooks.listen_port = static_cast<uint16_t>(std::stoi(*port_str));
  hooks.advertise_ip_override = advertise_ip;

  auto core = remboard::Core::create(hooks);

  core->set_on_incoming_item([](remboard::IncomingItem item) {
    std::cout << "ITEM kind=" << item_kind_name(item.kind)
              << " from=" << item.from_display_name
              << " envelope_id=" << item.envelope_id;
    if (item.kind == remboard::ItemKind::kFile) {
      std::cout << " file_name=" << item.file_name
                << " path=" << item.file_path
                << " size=" << item.file_size_bytes;
    } else {
      std::cout << " text=\"" << item.text << "\"";
    }
    std::cout << std::endl;
  });

  core->set_on_pairing_request([](remboard::PairingRequest req) {
    std::cout << "PAIRING_REQUEST device_uuid=" << req.device_uuid
              << " name=" << req.display_name
              << " fingerprint=" << req.fingerprint << std::endl;
  });

  core->set_on_transfer_progress([](remboard::TransferProgress p) {
    std::cout << "PROGRESS transfer_id=" << p.transfer_id << " "
              << p.chunks_done << "/" << p.total_chunks << std::endl;
  });

  core->set_on_peer_status_changed([](remboard::DeviceInfo d) {
    std::cout << "PEER_STATUS device_uuid=" << d.device_uuid
              << " online=" << (d.online ? "true" : "false") << std::endl;
  });

  core->start();
  auto self = core->self_info();
  std::cout << "READY device_uuid=" << self.device_uuid
            << " port=" << hooks.listen_port << std::endl;

  std::string line;
  while (std::getline(std::cin, line)) {
    auto parts = split(line);
    if (parts.empty()) continue;
    const std::string& cmd = parts[0];

    try {
      if (cmd == "pair") {
        std::cout << "QR " << core->begin_pairing() << std::endl;
      } else if (cmd == "pair-request" && parts.size() >= 2) {
        std::string json = line.substr(line.find(' ') + 1);
        core->request_pairing(json);
        std::cout << "OK pair-request sent" << std::endl;
      } else if (cmd == "accept" && parts.size() >= 2) {
        core->accept_pairing(parts[1]);
        std::cout << "OK accepted " << parts[1] << std::endl;
      } else if (cmd == "reject" && parts.size() >= 2) {
        core->reject_pairing(parts[1]);
        std::cout << "OK rejected " << parts[1] << std::endl;
      } else if (cmd == "send-text" && parts.size() >= 3) {
        std::string text = line.substr(line.find(parts[2]));
        std::string id = core->send_text(parts[1], text);
        std::cout << "OK sent envelope_id=" << id << std::endl;
      } else if (cmd == "send-file" && parts.size() >= 3) {
        std::string id = core->send_file(parts[1], parts[2]);
        std::cout << "OK sending transfer_id=" << id << std::endl;
      } else if (cmd == "act" && parts.size() >= 3) {
        remboard::ItemAction action;
        if (parts[2] == "copy")
          action = remboard::ItemAction::kCopy;
        else if (parts[2] == "reject")
          action = remboard::ItemAction::kReject;
        else
          action = remboard::ItemAction::kSave;
        std::string save_to = parts.size() >= 4 ? parts[3] : "";
        core->act_on_item(parts[1], action, save_to);
        std::cout << "OK act" << std::endl;
      } else if (cmd == "list") {
        for (const auto& d : core->list_paired_devices()) {
          std::cout << "DEVICE " << d.device_uuid << " " << d.display_name
                     << " " << d.last_known_ip << ":" << d.last_known_port
                     << " online=" << (d.online ? "true" : "false")
                     << std::endl;
        }
      } else if (cmd == "quit") {
        break;
      } else {
        std::cout << "ERR unknown command: " << cmd << std::endl;
      }
    } catch (const std::exception& e) {
      std::cout << "ERR " << e.what() << std::endl;
    }
  }

  core->shutdown();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) {
    print_usage();
    return 1;
  }
  std::string subcommand = args[0];
  args.erase(args.begin());

  if (subcommand == "identity") return run_identity(args);
  if (subcommand == "serve") return run_serve(args);

  print_usage();
  return 1;
}
