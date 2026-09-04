#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "remboard/idiscovery.h"

namespace remboard {

// Android IDiscovery implementation. Deliberately has no jni.h dependency
// (core stays free of JNI types) -- the JNI bridge (app-android/app/src/
// main/cpp/jni_bridge.cpp) constructs one of these, wires advertise/stop to
// lambdas that call into Kotlin's NsdDiscovery, and calls
// notify_peer_resolved() directly (a plain C++ call) whenever NsdDiscovery
// resolves a peer.
class AndroidDiscovery : public IDiscovery {
 public:
  using AdvertiseHook = std::function<void(
      const std::string& device_uuid, const std::string& pubkey_fingerprint,
      const std::string& platform, uint16_t port)>;
  using StopHook = std::function<void()>;

  void set_advertise_hook(AdvertiseHook hook);
  void set_stop_hook(StopHook hook);

  void notify_peer_resolved(const std::string& device_uuid,
                             const std::string& ip, uint16_t port);
  // Called by the JNI bridge when NsdDiscovery's onServiceLost fires for a
  // previously-resolved peer.
  void notify_peer_lost(const std::string& device_uuid);

  void advertise(const std::string& device_uuid,
                  const std::string& pubkey_fingerprint,
                  const std::string& platform, uint16_t port) override;
  void set_on_peer_resolved(PeerResolvedCallback callback) override;
  void set_on_peer_lost(PeerLostCallback callback) override;
  void stop() override;

 private:
  AdvertiseHook advertise_hook_;
  StopHook stop_hook_;
  PeerResolvedCallback on_peer_resolved_;
  PeerLostCallback on_peer_lost_;
  std::mutex mutex_;
};

}  // namespace remboard
