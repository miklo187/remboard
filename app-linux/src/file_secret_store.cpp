#include "file_secret_store.h"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace remboard_app {

namespace {

std::string default_config_dir() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME");
      xdg != nullptr && xdg[0] != '\0') {
    return std::string(xdg) + "/remboard";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::string(home) + "/.config/remboard";
  }
  if (struct passwd* pw = getpwuid(getuid()); pw != nullptr && pw->pw_dir != nullptr) {
    return std::string(pw->pw_dir) + "/.config/remboard";
  }
  throw std::runtime_error("cannot determine home directory for config storage");
}

}  // namespace

FileSecretStore::FileSecretStore() : FileSecretStore(default_config_dir()) {}

FileSecretStore::FileSecretStore(std::string config_dir)
    : config_dir_(std::move(config_dir)) {
  std::filesystem::create_directories(config_dir_);
  ::chmod(config_dir_.c_str(), S_IRWXU);
}

std::string FileSecretStore::path_for(const std::string& key) const {
  return config_dir_ + "/" + key + ".json";
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
  out.close();
  ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
}

}  // namespace remboard_app
