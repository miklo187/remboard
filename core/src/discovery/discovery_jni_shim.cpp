#include "remboard/platform/discovery_jni.h"

namespace remboard {

void AndroidDiscovery::set_advertise_hook(AdvertiseHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  advertise_hook_ = std::move(hook);
}

void AndroidDiscovery::set_stop_hook(StopHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  stop_hook_ = std::move(hook);
}

void AndroidDiscovery::notify_peer_resolved(const std::string& device_uuid,
                                             const std::string& ip,
                                             uint16_t port) {
  PeerResolvedCallback cb;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cb = on_peer_resolved_;
  }
  if (cb) cb(device_uuid, ip, port);
}

void AndroidDiscovery::advertise(const std::string& device_uuid,
                                  const std::string& pubkey_fingerprint,
                                  const std::string& platform,
                                  uint16_t port) {
  AdvertiseHook hook;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    hook = advertise_hook_;
  }
  if (hook) hook(device_uuid, pubkey_fingerprint, platform, port);
}

void AndroidDiscovery::set_on_peer_resolved(PeerResolvedCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_peer_resolved_ = std::move(callback);
}

void AndroidDiscovery::notify_peer_lost(const std::string& device_uuid) {
  PeerLostCallback cb;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cb = on_peer_lost_;
  }
  if (cb) cb(device_uuid);
}

void AndroidDiscovery::set_on_peer_lost(PeerLostCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_peer_lost_ = std::move(callback);
}

void AndroidDiscovery::stop() {
  StopHook hook;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    hook = stop_hook_;
  }
  if (hook) hook();
}

}  // namespace remboard
