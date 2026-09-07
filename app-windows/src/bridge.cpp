#include "bridge.h"

#include <windows.h>

#include <shlobj.h>
#include <shobjidl.h>

#include <nlohmann/json.hpp>

namespace remboard_app {

namespace {

// RAII COM init for the file-dialog calls below. The webview library's
// Win32 backend already calls CoInitializeEx(COINIT_APARTMENTTHREADED)
// itself when constructed, so this just bumps the same apartment's
// refcount (CoInitializeEx returns S_FALSE) rather than conflicting with it.
struct ComInit {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ~ComInit() {
    if (SUCCEEDED(hr)) CoUninitialize();
  }
};

std::string wide_to_utf8(const wchar_t* wide) {
  if (wide == nullptr || wide[0] == L'\0') return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<size_t>(size) - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), size, nullptr, nullptr);
  return result;
}

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), size);
  return result;
}

nlohmann::json device_to_json(const remboard::DeviceInfo& d) {
  nlohmann::json j;
  j["device_uuid"] = d.device_uuid;
  j["display_name"] = d.display_name;
  j["platform"] = remboard::to_string(d.platform);
  j["online"] = d.online;
  j["last_known_ip"] = d.last_known_ip;
  j["last_known_port"] = d.last_known_port;
  return j;
}

nlohmann::json item_to_json(const remboard::IncomingItem& item) {
  nlohmann::json j;
  j["envelope_id"] = item.envelope_id;
  j["from_device_uuid"] = item.from_device_uuid;
  j["from_display_name"] = item.from_display_name;
  switch (item.kind) {
    case remboard::ItemKind::kFile:
      j["kind"] = "file";
      break;
    case remboard::ItemKind::kClipboardText:
      j["kind"] = "clipboard";
      break;
    case remboard::ItemKind::kText:
    default:
      j["kind"] = "text";
      break;
  }
  j["received_at_unix_ms"] = item.received_at_unix_ms;
  j["text"] = item.text;
  j["source_app"] = item.source_app;
  j["file_name"] = item.file_name;
  j["mime_type"] = item.mime_type;
  j["file_size_bytes"] = item.file_size_bytes;
  return j;
}

nlohmann::json pairing_request_to_json(const remboard::PairingRequest& r) {
  nlohmann::json j;
  j["device_uuid"] = r.device_uuid;
  j["display_name"] = r.display_name;
  j["platform"] = remboard::to_string(r.platform);
  j["fingerprint"] = r.fingerprint;
  return j;
}

nlohmann::json progress_to_json(const remboard::TransferProgress& p) {
  nlohmann::json j;
  j["transfer_id"] = p.transfer_id;
  j["peer_device_uuid"] = p.peer_device_uuid;
  j["file_name"] = p.file_name;
  j["direction"] = p.direction == remboard::TransferDirection::kIncoming
                        ? "incoming"
                        : "outgoing";
  j["chunks_done"] = p.chunks_done;
  j["total_chunks"] = p.total_chunks;
  return j;
}

// Empty string means "no file chosen" (dialog cancelled).
std::string run_open_file_dialog() {
  ComInit com;
  std::string path;
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&dialog)))) {
    dialog->SetTitle(L"Select File to Send");
    if (SUCCEEDED(dialog->Show(nullptr))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR filepath = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &filepath))) {
          path = wide_to_utf8(filepath);
          CoTaskMemFree(filepath);
        }
        item->Release();
      }
    }
    dialog->Release();
  }
  return path;
}

std::string run_save_file_dialog(const std::string& suggested_name) {
  ComInit com;
  std::string path;
  IFileSaveDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&dialog)))) {
    dialog->SetTitle(L"Save Received File");
    dialog->SetOptions(FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    std::wstring wide_name = utf8_to_wide(suggested_name);
    dialog->SetFileName(wide_name.c_str());
    if (SUCCEEDED(dialog->Show(nullptr))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR filepath = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &filepath))) {
          path = wide_to_utf8(filepath);
          CoTaskMemFree(filepath);
        }
        item->Release();
      }
    }
    dialog->Release();
  }
  return path;
}

}  // namespace

Bridge::Bridge(remboard::Core& core, webview::webview& w)
    : core_(core), w_(w) {}

void Bridge::register_handlers() {
  w_.bind("getSelfInfo",
          [this](const std::string& req) { return get_self_info(req); });
  w_.bind("listDevices",
          [this](const std::string& req) { return list_devices(req); });
  w_.bind("beginPairing",
          [this](const std::string& req) { return begin_pairing(req); });
  w_.bind("cancelPairing",
          [this](const std::string& req) { return cancel_pairing(req); });
  w_.bind("requestPairing",
          [this](const std::string& req) { return request_pairing(req); });
  w_.bind("acceptPairing",
          [this](const std::string& req) { return accept_pairing(req); });
  w_.bind("rejectPairing",
          [this](const std::string& req) { return reject_pairing(req); });
  w_.bind("removeDevice",
          [this](const std::string& req) { return remove_device(req); });
  w_.bind("sendText",
          [this](const std::string& req) { return send_text(req); });
  w_.bind("pickAndSendFile", [this](const std::string& req) {
    return pick_and_send_file(req);
  });
  w_.bind("actOnItem",
          [this](const std::string& req) { return act_on_item(req); });
  w_.bind("saveItem",
          [this](const std::string& req) { return save_item(req); });
}

void Bridge::register_push_callbacks() {
  core_.set_on_incoming_item([this](remboard::IncomingItem item) {
    std::string json = item_to_json(item).dump();
    w_.dispatch(
        [this, json]() { w_.eval("window.onIncomingItem(" + json + ")"); });
  });

  core_.set_on_pairing_request([this](remboard::PairingRequest req) {
    std::string json = pairing_request_to_json(req).dump();
    w_.dispatch([this, json]() {
      w_.eval("window.onPairingRequest(" + json + ")");
    });
  });

  core_.set_on_transfer_progress([this](remboard::TransferProgress p) {
    std::string json = progress_to_json(p).dump();
    w_.dispatch([this, json]() {
      w_.eval("window.onTransferProgress(" + json + ")");
    });
  });

  core_.set_on_peer_status_changed([this](remboard::DeviceInfo d) {
    std::string json = device_to_json(d).dump();
    w_.dispatch([this, json]() {
      w_.eval("window.onPeerStatusChanged(" + json + ")");
    });
  });
}

std::string Bridge::get_self_info(const std::string&) {
  return device_to_json(core_.self_info()).dump();
}

std::string Bridge::list_devices(const std::string&) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& d : core_.list_paired_devices())
    arr.push_back(device_to_json(d));
  return arr.dump();
}

std::string Bridge::begin_pairing(const std::string&) {
  try {
    return core_.begin_pairing();
  } catch (const std::exception& e) {
    return nlohmann::json{{"error", e.what()}}.dump();
  }
}

std::string Bridge::cancel_pairing(const std::string&) {
  core_.cancel_pairing();
  return "null";
}

std::string Bridge::request_pairing(const std::string& req) {
  try {
    auto args = nlohmann::json::parse(req);
    core_.request_pairing(args.at(0).get<std::string>());
    return R"({"ok":true})";
  } catch (const std::exception& e) {
    return nlohmann::json{{"ok", false}, {"error", e.what()}}.dump();
  }
}

std::string Bridge::accept_pairing(const std::string& req) {
  auto args = nlohmann::json::parse(req);
  core_.accept_pairing(args.at(0).get<std::string>());
  return "null";
}

std::string Bridge::reject_pairing(const std::string& req) {
  auto args = nlohmann::json::parse(req);
  core_.reject_pairing(args.at(0).get<std::string>());
  return "null";
}

std::string Bridge::remove_device(const std::string& req) {
  auto args = nlohmann::json::parse(req);
  core_.remove_device(args.at(0).get<std::string>());
  return "null";
}

std::string Bridge::send_text(const std::string& req) {
  try {
    auto args = nlohmann::json::parse(req);
    std::string id = core_.send_text(args.at(0).get<std::string>(),
                                      args.at(1).get<std::string>());
    return nlohmann::json{{"ok", true}, {"envelope_id", id}}.dump();
  } catch (const std::exception& e) {
    return nlohmann::json{{"ok", false}, {"error", e.what()}}.dump();
  }
}

std::string Bridge::pick_and_send_file(const std::string& req) {
  try {
    auto args = nlohmann::json::parse(req);
    std::string device_uuid = args.at(0).get<std::string>();
    std::string path = run_open_file_dialog();
    if (path.empty())
      return nlohmann::json{{"ok", false}, {"cancelled", true}}.dump();
    std::string transfer_id = core_.send_file(device_uuid, path);
    return nlohmann::json{{"ok", true}, {"transfer_id", transfer_id}}.dump();
  } catch (const std::exception& e) {
    return nlohmann::json{{"ok", false}, {"error", e.what()}}.dump();
  }
}

std::string Bridge::act_on_item(const std::string& req) {
  try {
    auto args = nlohmann::json::parse(req);
    std::string envelope_id = args.at(0).get<std::string>();
    std::string action_str = args.at(1).get<std::string>();
    remboard::ItemAction action;
    if (action_str == "reject") {
      action = remboard::ItemAction::kReject;
    } else if (action_str == "dismiss") {
      action = remboard::ItemAction::kDismiss;
    } else {
      action = remboard::ItemAction::kCopy;
    }
    core_.act_on_item(envelope_id, action, "");
    return "null";
  } catch (const std::exception& e) {
    return nlohmann::json{{"error", e.what()}}.dump();
  }
}

std::string Bridge::save_item(const std::string& req) {
  try {
    auto args = nlohmann::json::parse(req);
    std::string envelope_id = args.at(0).get<std::string>();
    std::string suggested_name = args.at(1).get<std::string>();
    std::string path = run_save_file_dialog(suggested_name);
    if (path.empty())
      return nlohmann::json{{"ok", false}, {"cancelled", true}}.dump();
    core_.act_on_item(envelope_id, remboard::ItemAction::kSave, path);
    return nlohmann::json{{"ok", true}, {"path", path}}.dump();
  } catch (const std::exception& e) {
    return nlohmann::json{{"ok", false}, {"error", e.what()}}.dump();
  }
}

}  // namespace remboard_app
