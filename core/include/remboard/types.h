#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace remboard {

using PubKey = std::vector<uint8_t>;  // 32 raw Curve25519 bytes

enum class Platform { kUnknown, kLinux, kAndroid };

std::string to_string(Platform platform);
Platform platform_from_string(const std::string& s);

struct DeviceInfo {
  std::string device_uuid;
  std::string display_name;
  Platform platform = Platform::kUnknown;
  PubKey curve_pubkey;
  bool online = false;
  std::string last_known_ip;
  uint16_t last_known_port = 0;
  int64_t paired_at_unix_ms = 0;
};

enum class ItemKind { kText, kClipboardText, kFile };

struct IncomingItem {
  std::string envelope_id;
  std::string from_device_uuid;
  std::string from_display_name;
  ItemKind kind = ItemKind::kText;
  int64_t received_at_unix_ms = 0;

  // kText / kClipboardText
  std::string text;
  std::string source_app;  // kText only, may be empty

  // kFile — populated once the transfer completes; the file is already
  // written to a core-owned staging directory at file_path.
  std::string file_name;
  std::string file_path;
  std::string mime_type;
  int64_t file_size_bytes = 0;
};

struct PairingRequest {
  std::string device_uuid;
  std::string display_name;
  Platform platform = Platform::kUnknown;
  std::string fingerprint;  // short hex, for optional visual comparison
};

enum class TransferDirection { kIncoming, kOutgoing };

struct TransferProgress {
  std::string transfer_id;
  std::string peer_device_uuid;
  std::string file_name;
  TransferDirection direction = TransferDirection::kIncoming;
  int32_t chunks_done = 0;
  int32_t total_chunks = 0;
  bool failed = false;
  bool complete = false;
};

enum class ItemAction { kCopy, kSave, kReject, kDismiss };

}  // namespace remboard
