#pragma once

#include <optional>
#include <string>

namespace remboard {

// Platform-provided storage for small at-rest secrets/config blobs (own
// keypair, trusted-device list). Implementations are responsible for
// picking an appropriate storage medium and permissions/encryption:
//   - Linux: 0600 files under 0700 ~/.config/remboard/
//   - Android: Keystore-backed EncryptedSharedPreferences
//
// Values are opaque strings (core stores JSON); keys are simple identifiers
// like "identity" or "devices".
class ISecretStore {
 public:
  virtual ~ISecretStore() = default;

  virtual std::optional<std::string> load(const std::string& key) = 0;
  virtual void save(const std::string& key, const std::string& value) = 0;
};

}  // namespace remboard
