#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace remboard {

// Platform-provided LAN discovery (mDNS/Avahi on Linux, NsdManager on
// Android) used only to refresh a *paired* peer's current ip:port after the
// QR-pairing-time address may have changed. Implementations advertise this
// device's own service and resolve a specific peer by device_uuid.
class IDiscovery {
 public:
  using PeerResolvedCallback = std::function<void(
      const std::string& device_uuid, const std::string& ip, uint16_t port)>;
  // Fired when a previously-resolved instance's mDNS advertisement
  // disappears (clean shutdown "goodbye" packet, or record expiry) -- the
  // main signal core has for showing a peer as offline again, since
  // otherwise online status only ever latches true.
  using PeerLostCallback =
      std::function<void(const std::string& device_uuid)>;

  virtual ~IDiscovery() = default;

  // Starts advertising this device as `_remboard._tcp` on `port`, with
  // `device_uuid` / `pubkey_fingerprint` / `platform` published as TXT
  // records, and starts browsing for other instances.
  virtual void advertise(const std::string& device_uuid,
                          const std::string& pubkey_fingerprint,
                          const std::string& platform, uint16_t port) = 0;

  // Invoked whenever a browsed instance's TXT device_uuid matches a device
  // this process cares about (core filters to trusted peers itself; the
  // callback may fire for any discovered instance).
  virtual void set_on_peer_resolved(PeerResolvedCallback callback) = 0;

  virtual void set_on_peer_lost(PeerLostCallback callback) = 0;

  virtual void stop() = 0;
};

}  // namespace remboard
