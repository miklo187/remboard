#include "remboard/platform/discovery_avahi.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/address.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>
#include <avahi-common/thread-watch.h>

#include <mutex>
#include <string>
#include <unordered_map>

namespace remboard {
namespace {

constexpr const char* kServiceType = "_remboard._tcp";

class AvahiDiscovery : public IDiscovery {
 public:
  AvahiDiscovery() = default;
  ~AvahiDiscovery() override { stop(); }

  void advertise(const std::string& device_uuid,
                  const std::string& pubkey_fingerprint,
                  const std::string& platform, uint16_t port) override {
    device_uuid_ = device_uuid;
    pubkey_fingerprint_ = pubkey_fingerprint;
    platform_ = platform;
    port_ = port;
    ensure_started();
  }

  void set_on_peer_resolved(PeerResolvedCallback callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_peer_resolved_ = std::move(callback);
  }

  void set_on_peer_lost(PeerLostCallback callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    on_peer_lost_ = std::move(callback);
  }

  void stop() override {
    if (threaded_poll_) avahi_threaded_poll_stop(threaded_poll_);
    if (browser_) {
      avahi_service_browser_free(browser_);
      browser_ = nullptr;
    }
    if (group_) {
      avahi_entry_group_free(group_);
      group_ = nullptr;
    }
    if (client_) {
      avahi_client_free(client_);
      client_ = nullptr;
    }
    if (threaded_poll_) {
      avahi_threaded_poll_free(threaded_poll_);
      threaded_poll_ = nullptr;
    }
  }

 private:
  void ensure_started() {
    if (threaded_poll_) return;
    threaded_poll_ = avahi_threaded_poll_new();
    if (!threaded_poll_) return;
    int error = 0;
    client_ = avahi_client_new(avahi_threaded_poll_get(threaded_poll_),
                                static_cast<AvahiClientFlags>(0),
                                &AvahiDiscovery::client_callback, this,
                                &error);
    if (!client_) return;
    avahi_threaded_poll_start(threaded_poll_);
  }

  static void client_callback(AvahiClient* c, AvahiClientState state,
                               void* userdata) {
    auto* self = static_cast<AvahiDiscovery*>(userdata);
    if (state == AVAHI_CLIENT_S_RUNNING) {
      self->publish_service(c);
      self->start_browsing(c);
    }
  }

  void publish_service(AvahiClient* c) {
    if (!group_) {
      group_ = avahi_entry_group_new(
          c, &AvahiDiscovery::entry_group_callback, this);
    }
    if (!group_ || !avahi_entry_group_is_empty(group_)) return;

    AvahiStringList* txt = nullptr;
    txt = avahi_string_list_add_printf(txt, "device_uuid=%s",
                                        device_uuid_.c_str());
    txt = avahi_string_list_add_printf(txt, "pubkey_fp=%s",
                                        pubkey_fingerprint_.c_str());
    txt = avahi_string_list_add_printf(txt, "platform=%s", platform_.c_str());
    txt = avahi_string_list_add_printf(txt, "proto=1");

    std::string instance_name = "remboard-" + device_uuid_.substr(0, 8);
    avahi_entry_group_add_service_strlst(
        group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
        static_cast<AvahiPublishFlags>(0), instance_name.c_str(),
        kServiceType, nullptr, nullptr, port_, txt);
    avahi_string_list_free(txt);
    avahi_entry_group_commit(group_);
  }

  static void entry_group_callback(AvahiEntryGroup*, AvahiEntryGroupState,
                                    void*) {}

  void start_browsing(AvahiClient* c) {
    if (browser_) return;
    browser_ = avahi_service_browser_new(
        c, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, kServiceType, nullptr,
        static_cast<AvahiLookupFlags>(0), &AvahiDiscovery::browse_callback,
        this);
  }

  static void browse_callback(AvahiServiceBrowser*, AvahiIfIndex interface,
                               AvahiProtocol protocol,
                               AvahiBrowserEvent event, const char* name,
                               const char* type, const char* domain,
                               AvahiLookupResultFlags, void* userdata) {
    auto* self = static_cast<AvahiDiscovery*>(userdata);
    if (event == AVAHI_BROWSER_NEW) {
      avahi_service_resolver_new(
          self->client_, interface, protocol, name, type, domain,
          AVAHI_PROTO_UNSPEC, static_cast<AvahiLookupFlags>(0),
          &AvahiDiscovery::resolve_callback, self);
    } else if (event == AVAHI_BROWSER_REMOVE) {
      self->handle_instance_removed(name);
    }
  }

  void handle_instance_removed(const std::string& instance_name) {
    std::string lost_uuid;
    PeerLostCallback cb;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = instance_to_uuid_.find(instance_name);
      if (it == instance_to_uuid_.end()) return;
      lost_uuid = it->second;
      instance_to_uuid_.erase(it);
      cb = on_peer_lost_;
    }
    if (cb) cb(lost_uuid);
  }

  static void resolve_callback(AvahiServiceResolver* r, AvahiIfIndex,
                                AvahiProtocol, AvahiResolverEvent event,
                                const char* name, const char*, const char*,
                                const char*, const AvahiAddress* address,
                                uint16_t port, AvahiStringList* txt,
                                AvahiLookupResultFlags, void* userdata) {
    auto* self = static_cast<AvahiDiscovery*>(userdata);
    if (event == AVAHI_RESOLVER_FOUND) {
      char addr_str[AVAHI_ADDRESS_STR_MAX];
      avahi_address_snprint(addr_str, sizeof(addr_str), address);

      std::string resolved_uuid;
      AvahiStringList* item = avahi_string_list_find(txt, "device_uuid");
      if (item) {
        char* key = nullptr;
        char* value = nullptr;
        size_t value_size = 0;
        if (avahi_string_list_get_pair(item, &key, &value, &value_size) ==
            0) {
          resolved_uuid.assign(value, value_size);
          avahi_free(key);
          avahi_free(value);
        }
      }

      if (!resolved_uuid.empty() && resolved_uuid != self->device_uuid_) {
        PeerResolvedCallback cb;
        {
          std::lock_guard<std::mutex> lock(self->mutex_);
          self->instance_to_uuid_[name] = resolved_uuid;
          cb = self->on_peer_resolved_;
        }
        if (cb) cb(resolved_uuid, std::string(addr_str), port);
      }
    }
    avahi_service_resolver_free(r);
  }

  std::string device_uuid_;
  std::string pubkey_fingerprint_;
  std::string platform_;
  uint16_t port_ = 0;

  AvahiThreadedPoll* threaded_poll_ = nullptr;
  AvahiClient* client_ = nullptr;
  AvahiEntryGroup* group_ = nullptr;
  AvahiServiceBrowser* browser_ = nullptr;

  std::mutex mutex_;
  PeerResolvedCallback on_peer_resolved_;
  PeerLostCallback on_peer_lost_;
  std::unordered_map<std::string, std::string> instance_to_uuid_;
};

}  // namespace

std::unique_ptr<IDiscovery> make_avahi_discovery() {
  return std::make_unique<AvahiDiscovery>();
}

}  // namespace remboard
