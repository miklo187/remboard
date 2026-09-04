#include "identity/identity.h"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <regex>
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

}  // namespace

TEST(Identity, GenerateUuidV4LooksLikeAUuid) {
  std::string uuid = remboard::generate_uuid_v4();
  static const std::regex kUuidRegex(
      "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
  EXPECT_TRUE(std::regex_match(uuid, kUuidRegex)) << uuid;
}

TEST(Identity, GenerateUuidV4IsNotConstant) {
  EXPECT_NE(remboard::generate_uuid_v4(), remboard::generate_uuid_v4());
}

TEST(Identity, Base64RoundTrip) {
  std::vector<uint8_t> data = {0, 1, 2, 253, 254, 255, 'h', 'i'};
  std::string encoded = remboard::to_base64(data);
  EXPECT_EQ(remboard::from_base64(encoded), data);
}

TEST(Identity, HexRoundTrip) {
  std::vector<uint8_t> data = {0x00, 0xAB, 0xCD, 0xEF, 0xFF};
  std::string encoded = remboard::to_hex(data);
  EXPECT_EQ(encoded, "00abcdefff");
  EXPECT_EQ(remboard::from_hex(encoded), data);
}

TEST(Identity, FingerprintIsDeterministicAndShort) {
  std::vector<uint8_t> pubkey(32, 0x42);
  std::string fp1 = remboard::fingerprint_hex(pubkey);
  std::string fp2 = remboard::fingerprint_hex(pubkey);
  EXPECT_EQ(fp1, fp2);
  EXPECT_EQ(fp1.size(), 16u);  // 8 bytes -> 16 hex chars
}

TEST(Identity, FingerprintDiffersForDifferentKeys) {
  std::vector<uint8_t> a(32, 0x01);
  std::vector<uint8_t> b(32, 0x02);
  EXPECT_NE(remboard::fingerprint_hex(a), remboard::fingerprint_hex(b));
}

TEST(Identity, JsonRoundTrip) {
  remboard::Identity id;
  id.device_uuid = "test-uuid";
  id.display_name = "Test Device";
  id.platform = remboard::Platform::kAndroid;
  id.public_key = std::vector<uint8_t>(32, 0xAA);
  id.secret_key = std::vector<uint8_t>(32, 0xBB);

  remboard::Identity round_tripped = remboard::Identity::from_json(id.to_json());
  EXPECT_EQ(round_tripped.device_uuid, id.device_uuid);
  EXPECT_EQ(round_tripped.display_name, id.display_name);
  EXPECT_EQ(round_tripped.platform, id.platform);
  EXPECT_EQ(round_tripped.public_key, id.public_key);
  EXPECT_EQ(round_tripped.secret_key, id.secret_key);
}

TEST(Identity, LoadOrCreateIsStableAcrossCalls) {
  InMemorySecretStore store;
  auto first = remboard::load_or_create_identity(store, "My Device",
                                                   remboard::Platform::kLinux);
  auto second = remboard::load_or_create_identity(store, "Ignored Name",
                                                    remboard::Platform::kLinux);

  // Second call should load the already-persisted identity, not mint a new
  // one (and not overwrite it with the "Ignored Name" it was called with).
  EXPECT_EQ(first.device_uuid, second.device_uuid);
  EXPECT_EQ(first.public_key, second.public_key);
  EXPECT_EQ(second.display_name, "My Device");
}

TEST(Identity, LoadOrCreateGeneratesDistinctKeypairsForDifferentStores) {
  InMemorySecretStore store_a, store_b;
  auto a = remboard::load_or_create_identity(store_a, "A", remboard::Platform::kLinux);
  auto b = remboard::load_or_create_identity(store_b, "B", remboard::Platform::kLinux);

  EXPECT_NE(a.device_uuid, b.device_uuid);
  EXPECT_NE(a.public_key, b.public_key);
}
