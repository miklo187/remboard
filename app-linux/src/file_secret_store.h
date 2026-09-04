#pragma once

#include <optional>
#include <string>

#include "remboard/isecret_store.h"

namespace remboard_app {

// ISecretStore backed by flat JSON files under a 0700 directory (default
// ~/.config/remboard), each written 0600. Holds this device's own identity
// keypair ("identity") and the trusted-device registry ("devices").
class FileSecretStore : public remboard::ISecretStore {
 public:
  // Uses $XDG_CONFIG_HOME/remboard, falling back to ~/.config/remboard.
  FileSecretStore();
  explicit FileSecretStore(std::string config_dir);

  std::optional<std::string> load(const std::string& key) override;
  void save(const std::string& key, const std::string& value) override;

  const std::string& config_dir() const { return config_dir_; }

 private:
  std::string path_for(const std::string& key) const;

  std::string config_dir_;
};

}  // namespace remboard_app
