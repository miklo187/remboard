#include "remboard/platform/discovery_mdns.h"

// Winsock2.h must be included before windows.h (pulled in transitively by
// iphlpapi.h) to avoid the winsock.h/winsock2.h macro-redefinition clash.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <mdns.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace remboard {
namespace {

constexpr const char* kServiceName = "_remboard._tcp.local.";
constexpr int kMaxSockets = 32;
constexpr int kBrowseIntervalMs = 10000;

std::string ip_from_sockaddr(const struct sockaddr* addr, size_t addrlen) {
  char host[NI_MAXHOST] = {0};
  if (getnameinfo(addr, static_cast<socklen_t>(addrlen), host, sizeof(host), nullptr, 0,
                   NI_NUMERICHOST) != 0) {
    return {};
  }
  return host;
}

mdns_string_t to_mdns_string(const std::string& s) {
  return mdns_string_t{s.c_str(), s.size()};
}

// Enumerates this machine's non-loopback IPv4 addresses, opening one
// ephemeral-port send socket per interface (mirrors upstream mdns.c's
// open_client_sockets) and reporting the first such address as this
// device's own advertised address. IPv4-only: LAN discovery on Windows
// doesn't need IPv6 for this app.
int open_client_sockets(int* sockets, int max_sockets, struct sockaddr_in* out_first_address) {
  int num_sockets = 0;
  out_first_address->sin_family = 0;

  IP_ADAPTER_ADDRESSES* adapter_address = nullptr;
  ULONG address_size = 8000;
  ULONG ret;
  unsigned int num_retries = 4;
  do {
    adapter_address = static_cast<IP_ADAPTER_ADDRESSES*>(malloc(address_size));
    ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST, nullptr,
                                adapter_address, &address_size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
      free(adapter_address);
      adapter_address = nullptr;
      address_size *= 2;
    } else {
      break;
    }
  } while (num_retries-- > 0);

  if (!adapter_address || ret != NO_ERROR) {
    free(adapter_address);
    return num_sockets;
  }

  bool have_first = false;
  for (PIP_ADAPTER_ADDRESSES adapter = adapter_address; adapter; adapter = adapter->Next) {
    if (adapter->TunnelType == TUNNEL_TYPE_TEREDO) continue;
    if (adapter->OperStatus != IfOperStatusUp) continue;

    for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress; unicast;
         unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
      auto* saddr = reinterpret_cast<struct sockaddr_in*>(unicast->Address.lpSockaddr);
      if (saddr->sin_addr.S_un.S_un_b.s_b1 == 127) continue;  // loopback

      if (!have_first) {
        *out_first_address = *saddr;
        have_first = true;
      }
      if (num_sockets < max_sockets) {
        struct sockaddr_in bind_addr = *saddr;
        bind_addr.sin_port = 0;
        int sock = mdns_socket_open_ipv4(&bind_addr);
        if (sock >= 0) sockets[num_sockets++] = sock;
      }
    }
  }

  free(adapter_address);
  return num_sockets;
}

// Opens the single INADDR_ANY:5353 socket peers' queries about us arrive on.
int open_service_sockets(int* sockets, int max_sockets) {
  int num_sockets = 0;
  if (num_sockets < max_sockets) {
    struct sockaddr_in sock_addr;
    memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.sin_family = AF_INET;
    sock_addr.sin_addr.s_addr = INADDR_ANY;
    sock_addr.sin_port = htons(MDNS_PORT);
    int sock = mdns_socket_open_ipv4(&sock_addr);
    if (sock >= 0) sockets[num_sockets++] = sock;
  }
  return num_sockets;
}

// A peer's SRV and TXT records normally arrive together as additional
// records on the same reply, but may arrive in either order (or, for a
// unicast reply to our periodic query vs. a multicast one answering someone
// else's query, in separate packets) -- accumulate both halves per instance
// name and only report a peer once both are known.
struct PendingPeer {
  std::string ip;
  uint16_t port = 0;
  std::string device_uuid;
  bool has_srv = false;
  bool has_txt = false;
};

class MdnsDiscovery : public IDiscovery {
 public:
  MdnsDiscovery() = default;
  ~MdnsDiscovery() override { stop(); }

  void advertise(const std::string& device_uuid, const std::string& pubkey_fingerprint,
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
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
  }

 private:
  struct ServiceRecords {
    mdns_record_t ptr{};
    mdns_record_t srv{};
    mdns_record_t a{};
    mdns_record_t txt[4]{};
  };

  void ensure_started() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
  }

  void build_records() {
    mdns_string_t service_str = to_mdns_string(service_name_storage_);
    mdns_string_t instance_str = to_mdns_string(instance_name_);

    records_.ptr = mdns_record_t{};
    records_.ptr.name = service_str;
    records_.ptr.type = MDNS_RECORDTYPE_PTR;
    records_.ptr.data.ptr.name = instance_str;

    // The SRV target would normally be a separate "<host>.local." name;
    // this implementation never resolves it (the peer's IP is taken from
    // the reply packet's source address instead), so reusing the instance
    // name here is harmless and saves tracking a second name.
    records_.srv = mdns_record_t{};
    records_.srv.name = instance_str;
    records_.srv.type = MDNS_RECORDTYPE_SRV;
    records_.srv.data.srv.name = instance_str;
    records_.srv.data.srv.port = port_;
    records_.srv.data.srv.priority = 0;
    records_.srv.data.srv.weight = 0;

    records_.a = mdns_record_t{};
    records_.a.name = instance_str;
    records_.a.type = MDNS_RECORDTYPE_A;
    records_.a.data.a.addr = address_;

    static constexpr const char* kKeys[4] = {"device_uuid", "pubkey_fp", "platform", "proto"};
    const std::string* values[4] = {&device_uuid_, &pubkey_fingerprint_, &platform_,
                                     &proto_value_};
    for (int i = 0; i < 4; ++i) {
      records_.txt[i] = mdns_record_t{};
      records_.txt[i].name = instance_str;
      records_.txt[i].type = MDNS_RECORDTYPE_TXT;
      records_.txt[i].data.txt.key = mdns_string_t{kKeys[i], strlen(kKeys[i])};
      records_.txt[i].data.txt.value = to_mdns_string(*values[i]);
    }
  }

  // additional[] = SRV, A, then the 4 TXT records -- everything but the PTR
  // itself, used both for unsolicited announce/goodbye and as the
  // "additional" set alongside whichever record actually answers a query.
  size_t fill_additional(mdns_record_t* additional, bool include_srv, bool include_a) const {
    size_t n = 0;
    if (include_srv) additional[n++] = records_.srv;
    if (include_a) additional[n++] = records_.a;
    for (const auto& txt : records_.txt) additional[n++] = txt;
    return n;
  }

  void announce(int* sockets, int num_sockets, std::vector<char>& buffer, bool goodbye) const {
    mdns_record_t additional[6];
    size_t n = fill_additional(additional, /*include_srv=*/true, /*include_a=*/true);
    for (int i = 0; i < num_sockets; ++i) {
      if (goodbye) {
        mdns_goodbye_multicast(sockets[i], buffer.data(), buffer.size(), records_.ptr, nullptr, 0,
                                additional, n);
      } else {
        mdns_announce_multicast(sockets[i], buffer.data(), buffer.size(), records_.ptr, nullptr, 0,
                                 additional, n);
      }
    }
  }

  void send_browse_query(int* sockets, int num_sockets, std::vector<char>& buffer) const {
    for (int i = 0; i < num_sockets; ++i) {
      mdns_query_send(sockets[i], MDNS_RECORDTYPE_PTR, kServiceName, strlen(kServiceName),
                       buffer.data(), buffer.size(), 0);
    }
  }

  void send_answer(int sock, const struct sockaddr* from, size_t addrlen, bool unicast,
                    uint16_t query_id, uint16_t rtype, const mdns_string_t& queried_name,
                    const mdns_record_t& answer, const mdns_record_t* additional,
                    size_t additional_count, std::vector<char>& buffer) const {
    if (unicast) {
      mdns_query_answer_unicast(sock, from, addrlen, buffer.data(), buffer.size(), query_id,
                                 static_cast<mdns_record_type_t>(rtype), queried_name.str,
                                 queried_name.length, answer, nullptr, 0, additional,
                                 additional_count);
    } else {
      mdns_query_answer_multicast(sock, buffer.data(), buffer.size(), answer, nullptr, 0,
                                   additional, additional_count);
    }
  }

  // Handles incoming questions on the service (port 5353) sockets: other
  // peers (and this process's own periodic browse query, which is also
  // multicast and thus self-received) asking about our advertised service.
  static int on_service_query(int sock, const struct sockaddr* from, size_t addrlen,
                               mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                               uint16_t rclass, uint32_t /*ttl*/, const void* data, size_t size,
                               size_t name_offset, size_t /*name_length*/, size_t /*record_offset*/,
                               size_t /*record_length*/, void* user_data) {
    if (entry != MDNS_ENTRYTYPE_QUESTION) return 0;
    auto* self = static_cast<MdnsDiscovery*>(user_data);

    char namebuffer[256];
    size_t offset = name_offset;
    mdns_string_t name = mdns_string_extract(data, size, &offset, namebuffer, sizeof(namebuffer));
    std::string qname(name.str, name.length);
    bool unicast = (rclass & MDNS_UNICAST_RESPONSE) != 0;

    std::vector<char> send_buffer(1024);

    if (qname == kServiceName &&
        (rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY)) {
      mdns_record_t additional[6];
      size_t n = self->fill_additional(additional, /*include_srv=*/true, /*include_a=*/true);
      self->send_answer(sock, from, addrlen, unicast, query_id, rtype, name, self->records_.ptr,
                         additional, n, send_buffer);
    } else if (qname == self->instance_name_) {
      if (rtype == MDNS_RECORDTYPE_SRV || rtype == MDNS_RECORDTYPE_ANY) {
        mdns_record_t additional[6];
        size_t n = self->fill_additional(additional, /*include_srv=*/false, /*include_a=*/true);
        self->send_answer(sock, from, addrlen, unicast, query_id, rtype, name, self->records_.srv,
                           additional, n, send_buffer);
      } else if (rtype == MDNS_RECORDTYPE_A) {
        mdns_record_t additional[6];
        size_t n = self->fill_additional(additional, /*include_srv=*/true, /*include_a=*/false);
        self->send_answer(sock, from, addrlen, unicast, query_id, rtype, name, self->records_.a,
                           additional, n, send_buffer);
      }
    }
    return 0;
  }

  // Handles answers arriving on our own ephemeral query sockets: replies to
  // our periodic browse query, from every other _remboard._tcp instance on
  // the LAN (including peers we aren't paired with -- Core filters those).
  static int on_query_answer(int /*sock*/, const struct sockaddr* from, size_t addrlen,
                              mdns_entry_type_t entry, uint16_t /*query_id*/, uint16_t rtype,
                              uint16_t /*rclass*/, uint32_t ttl, const void* data, size_t size,
                              size_t name_offset, size_t /*name_length*/, size_t record_offset,
                              size_t record_length, void* user_data) {
    if (entry != MDNS_ENTRYTYPE_ANSWER && entry != MDNS_ENTRYTYPE_ADDITIONAL) return 0;
    auto* self = static_cast<MdnsDiscovery*>(user_data);

    char namebuffer[256];
    size_t offset = name_offset;
    mdns_string_t owner = mdns_string_extract(data, size, &offset, namebuffer, sizeof(namebuffer));
    std::string instance(owner.str, owner.length);

    if (rtype == MDNS_RECORDTYPE_PTR) {
      if (ttl == 0) {
        char target_buffer[256];
        mdns_string_t target = mdns_record_parse_ptr(data, size, record_offset, record_length,
                                                       target_buffer, sizeof(target_buffer));
        self->handle_goodbye(std::string(target.str, target.length));
      }
      return 0;
    }

    if (rtype == MDNS_RECORDTYPE_SRV) {
      char target_buffer[256];
      mdns_record_srv_t srv = mdns_record_parse_srv(data, size, record_offset, record_length,
                                                     target_buffer, sizeof(target_buffer));
      PendingPeer& peer = self->pending_[instance];
      peer.ip = ip_from_sockaddr(from, addrlen);
      peer.port = srv.port;
      peer.has_srv = true;
      if (peer.has_txt) self->try_emit(instance);
    } else if (rtype == MDNS_RECORDTYPE_TXT) {
      mdns_record_txt_t txt_buffer[16];
      size_t parsed = mdns_record_parse_txt(data, size, record_offset, record_length, txt_buffer,
                                             sizeof(txt_buffer) / sizeof(txt_buffer[0]));
      PendingPeer& peer = self->pending_[instance];
      for (size_t i = 0; i < parsed; ++i) {
        std::string key(txt_buffer[i].key.str, txt_buffer[i].key.length);
        if (key == "device_uuid") {
          peer.device_uuid.assign(txt_buffer[i].value.str, txt_buffer[i].value.length);
        }
      }
      peer.has_txt = true;
      if (peer.has_srv) self->try_emit(instance);
    }
    return 0;
  }

  void try_emit(const std::string& instance_name) {
    auto it = pending_.find(instance_name);
    if (it == pending_.end()) return;
    const PendingPeer& peer = it->second;
    if (peer.device_uuid.empty() || peer.device_uuid == device_uuid_) return;

    instance_to_uuid_[instance_name] = peer.device_uuid;

    PeerResolvedCallback cb;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cb = on_peer_resolved_;
    }
    if (cb) cb(peer.device_uuid, peer.ip, peer.port);
  }

  void handle_goodbye(const std::string& instance_name) {
    auto it = instance_to_uuid_.find(instance_name);
    if (it == instance_to_uuid_.end()) return;
    std::string uuid = it->second;
    instance_to_uuid_.erase(it);
    pending_.erase(instance_name);

    PeerLostCallback cb;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cb = on_peer_lost_;
    }
    if (cb) cb(uuid);
  }

  void run() {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
      running_ = false;
      return;
    }

    int service_sockets[kMaxSockets];
    int num_service_sockets = open_service_sockets(service_sockets, kMaxSockets);
    int client_sockets[kMaxSockets];
    struct sockaddr_in first_address {};
    int num_client_sockets = open_client_sockets(client_sockets, kMaxSockets, &first_address);

    if (num_service_sockets == 0 && num_client_sockets == 0) {
      WSACleanup();
      running_ = false;
      return;
    }

    address_ = first_address;
    instance_name_ = "remboard-" + device_uuid_.substr(0, 8) + "." + kServiceName;
    build_records();

    std::vector<char> buffer(2048);
    send_browse_query(client_sockets, num_client_sockets, buffer);
    announce(service_sockets, num_service_sockets, buffer, /*goodbye=*/false);

    auto last_browse = std::chrono::steady_clock::now();

    while (running_.load()) {
      fd_set readfs;
      FD_ZERO(&readfs);
      int nfds = 0;
      auto add_socket = [&](int sock) {
        FD_SET(sock, &readfs);
        if (sock >= nfds) nfds = sock + 1;
      };
      for (int i = 0; i < num_service_sockets; ++i) add_socket(service_sockets[i]);
      for (int i = 0; i < num_client_sockets; ++i) add_socket(client_sockets[i]);

      struct timeval timeout;
      timeout.tv_sec = 0;
      timeout.tv_usec = 200000;
      int res = select(nfds, &readfs, nullptr, nullptr, &timeout);
      if (res > 0) {
        for (int i = 0; i < num_service_sockets; ++i) {
          if (FD_ISSET(service_sockets[i], &readfs)) {
            mdns_socket_listen(service_sockets[i], buffer.data(), buffer.size(),
                                &MdnsDiscovery::on_service_query, this);
          }
        }
        for (int i = 0; i < num_client_sockets; ++i) {
          if (FD_ISSET(client_sockets[i], &readfs)) {
            mdns_query_recv(client_sockets[i], buffer.data(), buffer.size(),
                             &MdnsDiscovery::on_query_answer, this, 0);
          }
        }
      }

      auto now = std::chrono::steady_clock::now();
      if (now - last_browse > std::chrono::milliseconds(kBrowseIntervalMs)) {
        send_browse_query(client_sockets, num_client_sockets, buffer);
        last_browse = now;
      }
    }

    announce(service_sockets, num_service_sockets, buffer, /*goodbye=*/true);

    for (int i = 0; i < num_service_sockets; ++i) mdns_socket_close(service_sockets[i]);
    for (int i = 0; i < num_client_sockets; ++i) mdns_socket_close(client_sockets[i]);
    WSACleanup();
  }

  std::string device_uuid_;
  std::string pubkey_fingerprint_;
  std::string platform_;
  const std::string proto_value_ = "1";
  uint16_t port_ = 0;

  const std::string service_name_storage_ = kServiceName;
  std::string instance_name_;
  struct sockaddr_in address_ {};
  ServiceRecords records_;

  std::atomic<bool> running_{false};
  std::thread thread_;

  // pending_/instance_to_uuid_ are touched only from the mdns thread.
  std::unordered_map<std::string, PendingPeer> pending_;
  std::unordered_map<std::string, std::string> instance_to_uuid_;

  std::mutex mutex_;
  PeerResolvedCallback on_peer_resolved_;
  PeerLostCallback on_peer_lost_;
};

}  // namespace

std::unique_ptr<IDiscovery> make_mdns_discovery() {
  return std::make_unique<MdnsDiscovery>();
}

}  // namespace remboard
