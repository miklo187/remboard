#pragma once

#include <optional>
#include <string>

#include "remboard/isecret_store.h"

namespace remboard_app {

// ISecretStore backed by flat JSON files under %APPDATA%\remboard, one file
// per key. Holds this device's own identity keypair ("identity") and the
// trusted-device registry ("devices"). NTFS already restricts a user's
// %APPDATA% tree to that user, so unlike the Linux FileSecretStore there's
// no separate chmod-equivalent step here.
class FileSecretStore : public remboard::ISecretStore {
 public:
  // Uses %APPDATA%\remboard.
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
