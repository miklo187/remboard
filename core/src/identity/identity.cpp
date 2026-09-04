#include "identity/identity.h"

#include <sodium.h>

#include <array>
#include <cstdio>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace remboard {

namespace {
void ensure_sodium_init() {
  static const int rc = sodium_init();
  if (rc < 0) {
    throw std::runtime_error("libsodium initialization failed");
  }
}
}  // namespace

std::string generate_uuid_v4() {
  ensure_sodium_init();
  std::array<uint8_t, 16> bytes{};
  randombytes_buf(bytes.data(), bytes.size());
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);  // variant 10xx

  char out[37];
  std::snprintf(out, sizeof(out),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                "%02x%02x%02x%02x%02x%02x",
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                bytes[12], bytes[13], bytes[14], bytes[15]);
  return std::string(out);
}

std::string to_base64(const std::vector<uint8_t>& bytes) {
  ensure_sodium_init();
  std::string out(sodium_base64_ENCODED_LEN(bytes.size(),
                                             sodium_base64_VARIANT_ORIGINAL),
                   '\0');
  sodium_bin2base64(out.data(), out.size(), bytes.data(), bytes.size(),
                     sodium_base64_VARIANT_ORIGINAL);
  // sodium_base64_ENCODED_LEN includes the trailing NUL; trim it.
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

std::vector<uint8_t> from_base64(const std::string& b64) {
  ensure_sodium_init();
  std::vector<uint8_t> out(b64.size());
  size_t bin_len = 0;
  if (sodium_base642bin(out.data(), out.size(), b64.data(), b64.size(),
                         nullptr, &bin_len, nullptr,
                         sodium_base64_VARIANT_ORIGINAL) != 0) {
    throw std::runtime_error("invalid base64");
  }
  out.resize(bin_len);
  return out;
}

std::string to_hex(const std::vector<uint8_t>& bytes) {
  ensure_sodium_init();
  std::string out(bytes.size() * 2 + 1, '\0');
  sodium_bin2hex(out.data(), out.size(), bytes.data(), bytes.size());
  out.resize(bytes.size() * 2);
  return out;
}

std::vector<uint8_t> from_hex(const std::string& hex) {
  ensure_sodium_init();
  std::vector<uint8_t> out(hex.size() / 2);
  size_t bin_len = 0;
  if (sodium_hex2bin(out.data(), out.size(), hex.data(), hex.size(), nullptr,
                      &bin_len, nullptr) != 0) {
    throw std::runtime_error("invalid hex");
  }
  out.resize(bin_len);
  return out;
}

std::string fingerprint_hex(const PubKey& pubkey) {
  ensure_sodium_init();
  std::array<uint8_t, crypto_hash_sha256_BYTES> digest{};
  crypto_hash_sha256(digest.data(), pubkey.data(), pubkey.size());
  std::vector<uint8_t> short_digest(digest.begin(), digest.begin() + 8);
  return to_hex(short_digest);
}

std::string Identity::to_json() const {
  nlohmann::json j;
  j["device_uuid"] = device_uuid;
  j["display_name"] = display_name;
  j["platform"] = remboard::to_string(platform);
  j["public_key"] = to_base64(public_key);
  j["secret_key"] = to_base64(secret_key);
  return j.dump();
}

Identity Identity::from_json(const std::string& json) {
  nlohmann::json j = nlohmann::json::parse(json);
  Identity id;
  id.device_uuid = j.at("device_uuid").get<std::string>();
  id.display_name = j.at("display_name").get<std::string>();
  id.platform = platform_from_string(j.at("platform").get<std::string>());
  id.public_key = from_base64(j.at("public_key").get<std::string>());
  id.secret_key = from_base64(j.at("secret_key").get<std::string>());
  return id;
}

Identity load_or_create_identity(ISecretStore& store,
                                  const std::string& display_name,
                                  Platform platform) {
  if (auto existing = store.load("identity")) {
    return Identity::from_json(*existing);
  }

  ensure_sodium_init();
  Identity id;
  id.device_uuid = generate_uuid_v4();
  id.display_name = display_name;
  id.platform = platform;
  id.public_key.resize(crypto_box_PUBLICKEYBYTES);
  id.secret_key.resize(crypto_box_SECRETKEYBYTES);
  if (crypto_box_keypair(id.public_key.data(), id.secret_key.data()) != 0) {
    throw std::runtime_error("failed to generate CURVE keypair");
  }

  store.save("identity", id.to_json());
  return id;
}

}  // namespace remboard
