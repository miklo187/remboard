#include "store/device_store.h"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "identity/identity.h"

namespace remboard {

namespace {

nlohmann::json device_to_json(const DeviceInfo& d) {
  nlohmann::json j;
  j["device_uuid"] = d.device_uuid;
  j["display_name"] = d.display_name;
  j["platform"] = to_string(d.platform);
  j["curve_pubkey"] = to_base64(d.curve_pubkey);
  j["last_known_ip"] = d.last_known_ip;
  j["last_known_port"] = d.last_known_port;
  j["paired_at_unix_ms"] = d.paired_at_unix_ms;
  return j;
}

DeviceInfo device_from_json(const nlohmann::json& j) {
  DeviceInfo d;
  d.device_uuid = j.at("device_uuid").get<std::string>();
  d.display_name = j.at("display_name").get<std::string>();
  d.platform = platform_from_string(j.at("platform").get<std::string>());
  d.curve_pubkey = from_base64(j.at("curve_pubkey").get<std::string>());
  d.last_known_ip = j.value("last_known_ip", std::string());
  d.last_known_port =
      static_cast<uint16_t>(j.value("last_known_port", 0));
  d.paired_at_unix_ms = j.value("paired_at_unix_ms", int64_t{0});
  d.online = false;
  return d;
}

}  // namespace

DeviceStore::DeviceStore(ISecretStore& store) : store_(store) {
  if (auto existing = store_.load("devices")) {
    nlohmann::json arr = nlohmann::json::parse(*existing);
    for (const auto& item : arr) {
      devices_.push_back(device_from_json(item));
    }
  }
}

std::vector<DeviceInfo> DeviceStore::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return devices_;
}

std::optional<DeviceInfo> DeviceStore::find(
    const std::string& device_uuid) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(devices_.begin(), devices_.end(),
                          [&](const DeviceInfo& d) {
                            return d.device_uuid == device_uuid;
                          });
  if (it == devices_.end()) return std::nullopt;
  return *it;
}

std::optional<std::string> DeviceStore::find_by_pubkey(
    const PubKey& pubkey) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(
      devices_.begin(), devices_.end(),
      [&](const DeviceInfo& d) { return d.curve_pubkey == pubkey; });
  if (it == devices_.end()) return std::nullopt;
  return it->device_uuid;
}

void DeviceStore::upsert(const DeviceInfo& device) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(devices_.begin(), devices_.end(),
                          [&](const DeviceInfo& d) {
                            return d.device_uuid == device.device_uuid;
                          });
  if (it != devices_.end()) {
    *it = device;
  } else {
    devices_.push_back(device);
  }
  persist_locked();
}

void DeviceStore::remove(const std::string& device_uuid) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::erase_if(devices_, [&](const DeviceInfo& d) {
    return d.device_uuid == device_uuid;
  });
  persist_locked();
}

void DeviceStore::update_last_known_address(const std::string& device_uuid,
                                             const std::string& ip,
                                             uint16_t port) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(devices_.begin(), devices_.end(),
                          [&](const DeviceInfo& d) {
                            return d.device_uuid == device_uuid;
                          });
  if (it == devices_.end()) return;
  it->last_known_ip = ip;
  it->last_known_port = port;
  persist_locked();
}

void DeviceStore::persist_locked() {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& d : devices_) {
    arr.push_back(device_to_json(d));
  }
  store_.save("devices", arr.dump());
}

}  // namespace remboard
