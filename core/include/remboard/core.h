#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "remboard/idiscovery.h"
#include "remboard/isecret_store.h"
#include "remboard/types.h"

namespace remboard {

struct PlatformHooks {
  ISecretStore* secret_store = nullptr;  // not owned, must outlive Core
  IDiscovery* discovery = nullptr;       // not owned, must outlive Core
  std::string data_dir;                  // staging area for received files
  std::string display_name;              // this device's human-readable name
  Platform platform = Platform::kUnknown;
  uint16_t listen_port = 49321;

  // When non-empty, used verbatim as the "ip" field of QR pairing payloads
  // instead of auto-detecting a LAN address. Mainly for same-machine
  // testing (e.g. "127.0.0.1" for the CLI harness).
  std::string advertise_ip_override;
};

// Platform-agnostic core: owns identity, the ZeroMQ/CURVE transport, the
// trusted-device registry, pairing state machine, and file chunking. All
// types crossing this boundary are STL/POD -- no GTK/webview/JNI types.
//
// Callbacks registered via set_on_* fire on Core's internal background
// thread; callers are responsible for marshaling onto their own UI thread.
class Core {
 public:
  static std::unique_ptr<Core> create(PlatformHooks hooks);
  virtual ~Core() = default;

  virtual void start() = 0;
  virtual void shutdown() = 0;

  // Returns this device's own identity, useful for showing in "about"/debug
  // UI. Not the same as the paired-devices list.
  virtual DeviceInfo self_info() = 0;

  // Sends to an already-paired device. Returns the new envelope_id.
  // Throws std::runtime_error if to_device_uuid is not a paired/trusted
  // device.
  virtual std::string send_text(const std::string& to_device_uuid,
                                 const std::string& text,
                                 const std::string& source_app = "") = 0;
  virtual std::string send_file(const std::string& to_device_uuid,
                                 const std::string& file_path) = 0;

  // Pairing (see docs/PAIRING.md / plan for the full handshake).
  // Returns the QR payload JSON string to render.
  virtual std::string begin_pairing() = 0;
  virtual void cancel_pairing() = 0;
  // Called by the *requesting* side (the one that scanned a QR) after
  // decoding the QR JSON, to kick off the PairRequest handshake.
  virtual void request_pairing(const std::string& qr_payload_json) = 0;
  // Called by the *displaying* side once the user approves/rejects an
  // incoming PairingRequest surfaced via set_on_pairing_request.
  virtual void accept_pairing(const std::string& device_uuid) = 0;
  virtual void reject_pairing(const std::string& device_uuid) = 0;

  virtual std::vector<DeviceInfo> list_paired_devices() = 0;
  virtual void remove_device(const std::string& device_uuid) = 0;

  // Acts on a previously delivered IncomingItem (copy is a UI-local no-op
  // signal for symmetry; save persists file_path to a permanent location;
  // reject sends a Reject envelope back to the sender and discards staged
  // data).
  virtual void act_on_item(const std::string& envelope_id,
                            ItemAction action,
                            const std::string& save_to_path = "") = 0;

  virtual void set_on_incoming_item(
      std::function<void(IncomingItem)> callback) = 0;
  virtual void set_on_pairing_request(
      std::function<void(PairingRequest)> callback) = 0;
  virtual void set_on_transfer_progress(
      std::function<void(TransferProgress)> callback) = 0;
  virtual void set_on_peer_status_changed(
      std::function<void(DeviceInfo)> callback) = 0;
};

}  // namespace remboard
