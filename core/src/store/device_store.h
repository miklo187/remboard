#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "remboard/isecret_store.h"
#include "remboard/types.h"

namespace remboard {

// Thread-safe persisted registry of trusted (paired) devices. Backed by
// ISecretStore under the "devices" key as a JSON array. Safe to call from
// both Core's main logic and the ZAP handler thread (which needs to check
// is_trusted_pubkey on every inbound connection attempt).
class DeviceStore {
 public:
  explicit DeviceStore(ISecretStore& store);

  std::vector<DeviceInfo> list() const;
  std::optional<DeviceInfo> find(const std::string& device_uuid) const;

  // Returns the matching device_uuid if pubkey belongs to a trusted device.
  std::optional<std::string> find_by_pubkey(const PubKey& pubkey) const;

  void upsert(const DeviceInfo& device);
  void remove(const std::string& device_uuid);
  void update_last_known_address(const std::string& device_uuid,
                                  const std::string& ip, uint16_t port);

 private:
  void persist_locked();

  ISecretStore& store_;
  mutable std::mutex mutex_;
  std::vector<DeviceInfo> devices_;
};

}  // namespace remboard
