#include "store/device_store.h"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>

namespace {

class InMemorySecretStore : public remboard::ISecretStore {
 public:
  std::optional<std::string> load(const std::string& key) override {
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
  }
  void save(const std::string& key, const std::string& value) override {
    values_[key] = value;
  }

 private:
  std::map<std::string, std::string> values_;
};

remboard::DeviceInfo make_device(const std::string& uuid,
                                  const std::string& name,
                                  uint8_t pubkey_byte) {
  remboard::DeviceInfo d;
  d.device_uuid = uuid;
  d.display_name = name;
  d.platform = remboard::Platform::kLinux;
  d.curve_pubkey = std::vector<uint8_t>(32, pubkey_byte);
  d.last_known_ip = "192.168.1.42";
  d.last_known_port = 49321;
  d.paired_at_unix_ms = 1000;
  return d;
}

}  // namespace

TEST(DeviceStore, StartsEmpty) {
  InMemorySecretStore store;
  remboard::DeviceStore devices(store);
  EXPECT_TRUE(devices.list().empty());
}

TEST(DeviceStore, UpsertThenFind) {
  InMemorySecretStore store;
  remboard::DeviceStore devices(store);
  auto d = make_device("uuid-1", "Phone", 0x11);
  devices.upsert(d);

  auto found = devices.find("uuid-1");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->display_name, "Phone");
  EXPECT_EQ(found->curve_pubkey, d.curve_pubkey);
  EXPECT_EQ(found->last_known_ip, "192.168.1.42");
}

TEST(DeviceStore, FindMissingReturnsNullopt) {
  InMemorySecretStore store;
  remboard::DeviceStore devices(store);
  EXPECT_FALSE(devices.find("does-not-exist").has_value());
}

TEST(DeviceStore, UpsertUpdatesExistingEntryInPlace) {
  InMemorySecretStore store;
  remboard::DeviceStore devices(store);
  devices.upsert(make_device("uuid-1", "Phone", 0x11));

  auto updated = make_device("uuid-1", "Renamed Phone", 0x11);
  devices.upsert(updated);

  EXPECT_EQ(devices.list().size(), 1u);
  EXPECT_EQ(devices.find("uuid-1")->display_name, "Renamed Phone");
}

TEST(DeviceStore, FindByPubkey) {
  InMemorySecretStore store;
  remboard::DeviceStore devices(store);
  devices.upsert(make_device("uuid-1", "Phone", 0x11));
  devices.upsert(make_device("uuid-2", "Laptop", 0x22));

  auto uuid = devices.find_by_pubkey(std::vector<uint8_t>(32, 0x22));
  ASSERT_TRUE(uuid.has_value());
  EXPECT_EQ(*uuid, "uuid-2");

  EXPECT_FALSE(
      devices.find_by_pubkey(std::vector<uint8_t>(32, 0xFF)).has_value());
}

TEST(DeviceStore, RemoveDeletesEntry) {
  InMemorySecretStore store;
  remboard::DeviceStore devices(store);
  devices.upsert(make_device("uuid-1", "Phone", 0x11));
  devices.remove("uuid-1");
  EXPECT_FALSE(devices.find("uuid-1").has_value());
  EXPECT_TRUE(devices.list().empty());
}

TEST(DeviceStore, UpdateLastKnownAddress) {
  InMemorySecretStore store;
  remboard::DeviceStore devices(store);
  devices.upsert(make_device("uuid-1", "Phone", 0x11));

  devices.update_last_known_address("uuid-1", "10.0.0.5", 49322);

  auto found = devices.find("uuid-1");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->last_known_ip, "10.0.0.5");
  EXPECT_EQ(found->last_known_port, 49322);
}

TEST(DeviceStore, UpdateLastKnownAddressOnUnknownDeviceIsNoop) {
  InMemorySecretStore store;
  remboard::DeviceStore devices(store);
  devices.update_last_known_address("ghost", "10.0.0.5", 1234);
  EXPECT_TRUE(devices.list().empty());
}

TEST(DeviceStore, PersistsAcrossInstancesViaSameSecretStore) {
  InMemorySecretStore store;
  {
    remboard::DeviceStore devices(store);
    devices.upsert(make_device("uuid-1", "Phone", 0x11));
    devices.upsert(make_device("uuid-2", "Laptop", 0x22));
  }

  // A fresh DeviceStore over the same backing store should see the same
  // devices -- this is the persistence contract Core relies on across
  // process restarts.
  remboard::DeviceStore reloaded(store);
  EXPECT_EQ(reloaded.list().size(), 2u);
  ASSERT_TRUE(reloaded.find("uuid-1").has_value());
  EXPECT_EQ(reloaded.find("uuid-1")->display_name, "Phone");
  ASSERT_TRUE(reloaded.find("uuid-2").has_value());
  EXPECT_EQ(reloaded.find("uuid-2")->display_name, "Laptop");
}
