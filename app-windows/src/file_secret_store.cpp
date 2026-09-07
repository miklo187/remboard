#include "file_secret_store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace remboard_app {

namespace {

std::string default_config_dir() {
  if (const char* appdata = std::getenv("APPDATA");
      appdata != nullptr && appdata[0] != '\0') {
    return std::string(appdata) + "\\remboard";
  }
  throw std::runtime_error("cannot determine %APPDATA% for config storage");
}

}  // namespace

FileSecretStore::FileSecretStore() : FileSecretStore(default_config_dir()) {}

FileSecretStore::FileSecretStore(std::string config_dir)
    : config_dir_(std::move(config_dir)) {
  std::filesystem::create_directories(config_dir_);
}

std::string FileSecretStore::path_for(const std::string& key) const {
  return config_dir_ + "\\" + key + ".json";
}

std::optional<std::string> FileSecretStore::load(const std::string& key) {
  std::ifstream in(path_for(key), std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void FileSecretStore::save(const std::string& key, const std::string& value) {
  std::string path = path_for(key);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot write " + path);
  out << value;
}

}  // namespace remboard_app
