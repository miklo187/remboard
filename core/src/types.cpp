#include "remboard/types.h"

namespace remboard {

std::string to_string(Platform platform) {
  switch (platform) {
    case Platform::kLinux:
      return "linux";
    case Platform::kAndroid:
      return "android";
    case Platform::kWindows:
      return "windows";
    case Platform::kUnknown:
    default:
      return "unknown";
  }
}

Platform platform_from_string(const std::string& s) {
  if (s == "linux") return Platform::kLinux;
  if (s == "android") return Platform::kAndroid;
  if (s == "windows") return Platform::kWindows;
  return Platform::kUnknown;
}

}  // namespace remboard
