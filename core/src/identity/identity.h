#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "remboard/isecret_store.h"
#include "remboard/types.h"

namespace remboard {

struct Identity {
  std::string device_uuid;
  std::string display_name;
  Platform platform = Platform::kUnknown;
  PubKey public_key;                // 32 bytes, Curve25519
  std::vector<uint8_t> secret_key;  // 32 bytes, Curve25519

  std::string to_json() const;
  static Identity from_json(const std::string& json);
};

// Loads identity from secret_store["identity"], creating and persisting a
// fresh one (new UUID + Curve25519 keypair) if none exists yet.
Identity load_or_create_identity(ISecretStore& store,
                                  const std::string& display_name,
                                  Platform platform);

std::string generate_uuid_v4();

// Short hex fingerprint of a public key (first 8 bytes of SHA-256), for
// human comparison during pairing.
std::string fingerprint_hex(const PubKey& pubkey);

std::string to_base64(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> from_base64(const std::string& b64);
std::string to_hex(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> from_hex(const std::string& hex);

}  // namespace remboard
