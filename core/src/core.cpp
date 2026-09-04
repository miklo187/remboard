#include "remboard/core.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sodium.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "identity/identity.h"
#include "remboard.pb.h"
#include "store/device_store.h"
#include "transfer/chunker.h"
#include "transport/zmq_transport.h"

namespace remboard {

namespace {

int64_t now_unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string detect_local_ip() {
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return "127.0.0.1";

  std::string result;
  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
      continue;

    char buf[INET_ADDRSTRLEN];
    auto* addr_in = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
    inet_ntop(AF_INET, &addr_in->sin_addr, buf, sizeof(buf));
    std::string ip(buf);
    if (ip.rfind("169.254.", 0) == 0) continue;  // link-local
    result = ip;
    break;
  }
  freeifaddrs(ifaddr);
  return result.empty() ? "127.0.0.1" : result;
}

constexpr int64_t kPairingTtlMs = 120'000;

}  // namespace

class CoreImpl : public Core {
 public:
  explicit CoreImpl(PlatformHooks hooks)
      : hooks_(std::move(hooks)),
        identity_(load_or_create_identity(*hooks_.secret_store,
                                           hooks_.display_name,
                                           hooks_.platform)),
        device_store_(*hooks_.secret_store),
        transport_(
            identity_.public_key, identity_.secret_key, hooks_.listen_port,
            [this](const std::vector<uint8_t>& pk) {
              return zap_is_allowed(pk);
            }) {
    if (int rc = sodium_init(); rc < 0) {
      throw std::runtime_error("libsodium initialization failed");
    }
  }

  void start() override {
    transport_.set_on_message(
        [this](const std::string& sender_uuid, const std::string& peer_ip,
               const std::vector<uint8_t>& payload) {
          on_transport_message(sender_uuid, peer_ip, payload);
        });
    transport_.start();

    if (hooks_.discovery) {
      hooks_.discovery->set_on_peer_resolved(
          [this](const std::string& device_uuid, const std::string& ip,
                 uint16_t port) {
            device_store_.update_last_known_address(device_uuid, ip, port);
            mark_peer_online(device_uuid);
          });
      hooks_.discovery->set_on_peer_lost(
          [this](const std::string& device_uuid) {
            mark_peer_offline(device_uuid);
          });
      hooks_.discovery->advertise(identity_.device_uuid,
                                   fingerprint_hex(identity_.public_key),
                                   to_string(identity_.platform),
                                   hooks_.listen_port);
    }
  }

  void shutdown() override {
    transport_.stop();
    if (hooks_.discovery) hooks_.discovery->stop();
  }

  DeviceInfo self_info() override {
    DeviceInfo d;
    d.device_uuid = identity_.device_uuid;
    d.display_name = identity_.display_name;
    d.platform = identity_.platform;
    d.curve_pubkey = identity_.public_key;
    d.online = true;
    return d;
  }

  std::string send_text(const std::string& to_device_uuid,
                         const std::string& text,
                         const std::string& source_app) override {
    auto dev = device_store_.find(to_device_uuid);
    if (!dev) throw std::runtime_error("not a paired device: " + to_device_uuid);

    remboard::v1::Envelope env;
    fill_envelope_header(env);
    auto* ts = env.mutable_text_share();
    ts->set_text(text);
    ts->set_source_app(source_app);

    send_envelope_to_device(*dev, env);
    return env.envelope_id();
  }

  std::string send_file(const std::string& to_device_uuid,
                         const std::string& file_path) override {
    namespace fs = std::filesystem;
    auto dev = device_store_.find(to_device_uuid);
    if (!dev) throw std::runtime_error("not a paired device: " + to_device_uuid);
    if (!fs::exists(file_path))
      throw std::runtime_error("file not found: " + file_path);

    int64_t size = static_cast<int64_t>(fs::file_size(file_path));
    int32_t total_chunks = total_chunks_for_size(size);
    std::string transfer_id = generate_uuid_v4();
    std::string file_name = fs::path(file_path).filename().string();

    {
      remboard::v1::Envelope env;
      fill_envelope_header(env);
      auto* start = env.mutable_file_start();
      start->set_transfer_id(transfer_id);
      start->set_file_name(file_name);
      start->set_total_size_bytes(size);
      start->set_mime_type("");
      start->set_total_chunks(total_chunks);
      start->set_chunk_size_bytes(kChunkSizeBytes);
      send_envelope_to_device(*dev, env);
    }

    for (int32_t i = 0; i < total_chunks; ++i) {
      auto chunk = read_file_chunk(file_path, i);
      remboard::v1::Envelope env;
      fill_envelope_header(env);
      auto* fc = env.mutable_file_chunk();
      fc->set_transfer_id(transfer_id);
      fc->set_index(i);
      fc->set_data(chunk.data(), chunk.size());
      send_envelope_to_device(*dev, env);

      if (on_transfer_progress_) {
        TransferProgress p;
        p.transfer_id = transfer_id;
        p.peer_device_uuid = to_device_uuid;
        p.file_name = file_name;
        p.direction = TransferDirection::kOutgoing;
        p.chunks_done = i + 1;
        p.total_chunks = total_chunks;
        on_transfer_progress_(p);
      }
    }

    {
      remboard::v1::Envelope env;
      fill_envelope_header(env);
      auto* end = env.mutable_file_end();
      end->set_transfer_id(transfer_id);
      end->set_sha256_checksum(sha256_file_hex(file_path));
      send_envelope_to_device(*dev, env);
    }

    return transfer_id;
  }

  std::string begin_pairing() override {
    std::vector<uint8_t> nonce_bytes(16);
    randombytes_buf(nonce_bytes.data(), nonce_bytes.size());
    std::string nonce_b64 = to_base64(nonce_bytes);

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      current_pairing_nonce_ = nonce_b64;
    }
    pairing_started_at_ms_ = now_unix_ms();
    pairing_mode_active_.store(true);

    std::string ip = hooks_.advertise_ip_override.empty()
                          ? detect_local_ip()
                          : hooks_.advertise_ip_override;

    nlohmann::json j;
    j["v"] = 1;
    j["device_uuid"] = identity_.device_uuid;
    j["display_name"] = identity_.display_name;
    j["pubkey"] = to_base64(identity_.public_key);
    j["ip"] = ip;
    j["port"] = hooks_.listen_port;
    j["nonce"] = nonce_b64;
    return j.dump();
  }

  void cancel_pairing() override { pairing_mode_active_.store(false); }

  void request_pairing(const std::string& qr_payload_json) override {
    nlohmann::json j = nlohmann::json::parse(qr_payload_json);
    std::string peer_uuid = j.at("device_uuid").get<std::string>();
    PubKey peer_pubkey = from_base64(j.at("pubkey").get<std::string>());
    std::string ip = j.at("ip").get<std::string>();
    uint16_t port = static_cast<uint16_t>(j.at("port").get<int>());
    std::string nonce = j.at("nonce").get<std::string>();

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      outgoing_pairing_endpoints_[peer_uuid] = {ip, port};
    }

    std::string own_ip = hooks_.advertise_ip_override.empty()
                              ? detect_local_ip()
                              : hooks_.advertise_ip_override;

    remboard::v1::Envelope env;
    fill_envelope_header(env);
    auto* pr = env.mutable_pair_request();
    pr->set_qr_nonce(nonce);
    pr->set_fingerprint(fingerprint_hex(identity_.public_key));
    pr->set_requester_ip(own_ip);
    pr->set_requester_port(hooks_.listen_port);
    fill_device_id(*pr->mutable_requester());

    std::string bytes;
    (void)env.SerializeToString(&bytes);
    transport_.send_to_peer(identity_.device_uuid, peer_uuid, peer_pubkey, ip,
                             port,
                             std::vector<uint8_t>(bytes.begin(), bytes.end()));
  }

  void accept_pairing(const std::string& device_uuid) override {
    std::optional<PendingPairingInfo> pending = take_pending(device_uuid);
    if (!pending) return;

    DeviceInfo dev = pending->requester;
    dev.last_known_ip = pending->peer_ip;
    dev.paired_at_unix_ms = now_unix_ms();
    device_store_.upsert(dev);

    remboard::v1::Envelope env;
    fill_envelope_header(env);
    auto* resp = env.mutable_pair_response();
    resp->set_accepted(true);
    resp->set_fingerprint(fingerprint_hex(identity_.public_key));
    fill_device_id(*resp->mutable_responder());

    std::string bytes;
    (void)env.SerializeToString(&bytes);
    transport_.reply_via_router(
        pending->routing_id, std::vector<uint8_t>(bytes.begin(), bytes.end()));

    mark_peer_online(device_uuid);
  }

  void reject_pairing(const std::string& device_uuid) override {
    std::optional<PendingPairingInfo> pending = take_pending(device_uuid);
    if (!pending) return;

    remboard::v1::Envelope env;
    fill_envelope_header(env);
    auto* resp = env.mutable_pair_response();
    resp->set_accepted(false);
    resp->set_fingerprint(fingerprint_hex(identity_.public_key));
    fill_device_id(*resp->mutable_responder());

    std::string bytes;
    (void)env.SerializeToString(&bytes);
    transport_.reply_via_router(
        pending->routing_id, std::vector<uint8_t>(bytes.begin(), bytes.end()));
  }

  std::vector<DeviceInfo> list_paired_devices() override {
    auto devices = device_store_.list();
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& d : devices) {
      auto it = online_status_.find(d.device_uuid);
      d.online = (it != online_status_.end()) && it->second;
    }
    return devices;
  }

  void remove_device(const std::string& device_uuid) override {
    device_store_.remove(device_uuid);
  }

  void act_on_item(const std::string& envelope_id, ItemAction action,
                    const std::string& save_to_path) override {
    namespace fs = std::filesystem;
    std::optional<IncomingItem> item;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      auto it = received_items_.find(envelope_id);
      if (it == received_items_.end()) return;
      item = it->second;
      if (action != ItemAction::kCopy) received_items_.erase(it);
    }

    switch (action) {
      case ItemAction::kCopy:
        break;  // text already delivered via callback; platform layer owns
                // the OS clipboard write
      case ItemAction::kSave:
        if (item->kind == ItemKind::kFile && !save_to_path.empty()) {
          std::error_code ec;
          fs::copy_file(item->file_path, save_to_path,
                         fs::copy_options::overwrite_existing, ec);
          if (!ec) fs::remove(item->file_path, ec);
        }
        break;
      case ItemAction::kReject: {
        if (item->kind == ItemKind::kFile) {
          std::error_code ec;
          fs::remove(item->file_path, ec);
        }
        if (auto dev = device_store_.find(item->from_device_uuid)) {
          remboard::v1::Envelope env;
          fill_envelope_header(env);
          auto* rej = env.mutable_reject();
          rej->set_reject_envelope_id(envelope_id);
          rej->set_reason("user_rejected");
          send_envelope_to_device(*dev, env);
        }
        break;
      }
      case ItemAction::kDismiss:
        // Just clears it from the inbox (e.g. after the user already
        // copied/saved it) -- unlike kReject, no message is sent to the
        // sender.
        if (item->kind == ItemKind::kFile) {
          std::error_code ec;
          fs::remove(item->file_path, ec);
        }
        break;
    }
  }

  void set_on_incoming_item(std::function<void(IncomingItem)> cb) override {
    on_incoming_item_ = std::move(cb);
  }
  void set_on_pairing_request(
      std::function<void(PairingRequest)> cb) override {
    on_pairing_request_ = std::move(cb);
  }
  void set_on_transfer_progress(
      std::function<void(TransferProgress)> cb) override {
    on_transfer_progress_ = std::move(cb);
  }
  void set_on_peer_status_changed(
      std::function<void(DeviceInfo)> cb) override {
    on_peer_status_changed_ = std::move(cb);
  }

 private:
  struct PendingPairingInfo {
    DeviceInfo requester;
    std::string routing_id;
    std::string peer_ip;
  };

  bool zap_is_allowed(const std::vector<uint8_t>& pubkey) {
    if (device_store_.find_by_pubkey(pubkey).has_value()) return true;
    return pairing_mode_active_.load();
  }

  void fill_device_id(remboard::v1::DeviceId& out) {
    out.set_device_uuid(identity_.device_uuid);
    out.set_curve_pubkey(identity_.public_key.data(),
                          identity_.public_key.size());
    out.set_display_name(identity_.display_name);
    out.set_platform(to_string(identity_.platform));
  }

  void fill_envelope_header(remboard::v1::Envelope& env) {
    fill_device_id(*env.mutable_sender());
    env.set_envelope_id(generate_uuid_v4());
    env.set_sent_at_unix_ms(now_unix_ms());
  }

  void send_envelope_to_device(const DeviceInfo& dev,
                                remboard::v1::Envelope& env) {
    std::string bytes;
    (void)env.SerializeToString(&bytes);
    transport_.send_to_peer(identity_.device_uuid, dev.device_uuid,
                             dev.curve_pubkey, dev.last_known_ip,
                             dev.last_known_port,
                             std::vector<uint8_t>(bytes.begin(), bytes.end()));
  }

  void send_ack(const std::string& to_routing_id,
                const std::string& ack_for_envelope_id) {
    remboard::v1::Envelope env;
    fill_envelope_header(env);
    env.mutable_ack()->set_ack_envelope_id(ack_for_envelope_id);
    std::string bytes;
    (void)env.SerializeToString(&bytes);
    transport_.reply_via_router(
        to_routing_id, std::vector<uint8_t>(bytes.begin(), bytes.end()));
  }

  std::optional<PendingPairingInfo> take_pending(
      const std::string& device_uuid) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = pending_pairings_.find(device_uuid);
    if (it == pending_pairings_.end()) return std::nullopt;
    auto info = it->second;
    pending_pairings_.erase(it);
    return info;
  }

  void mark_peer_online(const std::string& device_uuid) {
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      auto it = online_status_.find(device_uuid);
      changed = (it == online_status_.end()) || !it->second;
      online_status_[device_uuid] = true;
    }
    if (changed && on_peer_status_changed_) {
      if (auto dev = device_store_.find(device_uuid)) {
        dev->online = true;
        on_peer_status_changed_(*dev);
      }
    }
  }

  void mark_peer_offline(const std::string& device_uuid) {
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      auto it = online_status_.find(device_uuid);
      changed = it != online_status_.end() && it->second;
      online_status_[device_uuid] = false;
    }
    if (changed && on_peer_status_changed_) {
      if (auto dev = device_store_.find(device_uuid)) {
        dev->online = false;
        on_peer_status_changed_(*dev);
      }
    }
  }

  void on_transport_message(const std::string& sender_uuid,
                             const std::string& peer_ip,
                             const std::vector<uint8_t>& payload) {
    remboard::v1::Envelope env;
    if (!env.ParseFromArray(payload.data(),
                             static_cast<int>(payload.size()))) {
      return;
    }

    mark_peer_online(sender_uuid);

    bool should_ack = true;
    switch (env.payload_case()) {
      case remboard::v1::Envelope::kPairRequest:
        handle_pair_request(env, sender_uuid, peer_ip);
        should_ack = false;
        break;
      case remboard::v1::Envelope::kPairResponse:
        handle_pair_response(env);
        should_ack = false;
        break;
      case remboard::v1::Envelope::kTextShare:
        handle_text_share(env);
        break;
      case remboard::v1::Envelope::kClipboardText:
        handle_clipboard_text(env);
        break;
      case remboard::v1::Envelope::kFileStart:
        handle_file_start(env);
        break;
      case remboard::v1::Envelope::kFileChunk:
        handle_file_chunk(env);
        break;
      case remboard::v1::Envelope::kFileEnd:
        handle_file_end(env);
        break;
      case remboard::v1::Envelope::kReject:
        should_ack = false;
        break;
      case remboard::v1::Envelope::kAck:
      case remboard::v1::Envelope::kStatus:
      default:
        should_ack = false;
        break;
    }

    if (should_ack) send_ack(sender_uuid, env.envelope_id());
  }

  void handle_pair_request(const remboard::v1::Envelope& env,
                            const std::string& sender_uuid,
                            const std::string& peer_ip) {
    const auto& pr = env.pair_request();
    PairingRequest surfaced;
    bool valid = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (pairing_mode_active_.load() &&
          now_unix_ms() - pairing_started_at_ms_ <= kPairingTtlMs &&
          pr.qr_nonce() == current_pairing_nonce_) {
        valid = true;
        pairing_mode_active_.store(false);  // one-shot

        DeviceInfo requester;
        requester.device_uuid = pr.requester().device_uuid();
        requester.display_name = pr.requester().display_name();
        requester.platform = platform_from_string(pr.requester().platform());
        const auto& pk = pr.requester().curve_pubkey();
        requester.curve_pubkey.assign(pk.begin(), pk.end());
        requester.last_known_port =
            pr.requester_port() != 0
                ? static_cast<uint16_t>(pr.requester_port())
                : hooks_.listen_port;

        std::string learned_ip =
            !pr.requester_ip().empty() ? pr.requester_ip() : peer_ip;

        PendingPairingInfo info;
        info.requester = requester;
        info.routing_id = sender_uuid;
        info.peer_ip = learned_ip;
        pending_pairings_[requester.device_uuid] = info;

        surfaced.device_uuid = requester.device_uuid;
        surfaced.display_name = requester.display_name;
        surfaced.platform = requester.platform;
        surfaced.fingerprint = pr.fingerprint();
      }
    }
    if (valid && on_pairing_request_) on_pairing_request_(surfaced);
  }

  void handle_pair_response(const remboard::v1::Envelope& env) {
    const auto& resp = env.pair_response();
    if (!resp.accepted()) return;

    DeviceInfo dev;
    dev.device_uuid = resp.responder().device_uuid();
    dev.display_name = resp.responder().display_name();
    dev.platform = platform_from_string(resp.responder().platform());
    const auto& pk = resp.responder().curve_pubkey();
    dev.curve_pubkey.assign(pk.begin(), pk.end());
    dev.paired_at_unix_ms = now_unix_ms();

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      auto it = outgoing_pairing_endpoints_.find(dev.device_uuid);
      if (it != outgoing_pairing_endpoints_.end()) {
        dev.last_known_ip = it->second.first;
        dev.last_known_port = it->second.second;
        outgoing_pairing_endpoints_.erase(it);
      }
    }

    device_store_.upsert(dev);
    mark_peer_online(dev.device_uuid);
  }

  void handle_text_share(const remboard::v1::Envelope& env) {
    IncomingItem item;
    item.envelope_id = env.envelope_id();
    item.from_device_uuid = env.sender().device_uuid();
    item.from_display_name = env.sender().display_name();
    item.kind = ItemKind::kText;
    item.received_at_unix_ms = now_unix_ms();
    item.text = env.text_share().text();
    item.source_app = env.text_share().source_app();
    deliver_item(item);
  }

  void handle_clipboard_text(const remboard::v1::Envelope& env) {
    IncomingItem item;
    item.envelope_id = env.envelope_id();
    item.from_device_uuid = env.sender().device_uuid();
    item.from_display_name = env.sender().display_name();
    item.kind = ItemKind::kClipboardText;
    item.received_at_unix_ms = now_unix_ms();
    item.text = env.clipboard_text().text();
    deliver_item(item);
  }

  void handle_file_start(const remboard::v1::Envelope& env) {
    namespace fs = std::filesystem;
    const auto& fs_msg = env.file_start();
    fs::path dir = fs::path(hooks_.data_dir) / "incoming";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path dest = dir / (fs_msg.transfer_id() + "_" + fs_msg.file_name());

    std::lock_guard<std::mutex> lock(state_mutex_);
    file_transfers_[fs_msg.transfer_id()] = std::make_unique<FileReceiveState>(
        fs_msg.transfer_id(), dest.string(), fs_msg.total_chunks(),
        fs_msg.chunk_size_bytes());
    FileTransferMeta meta;
    meta.from_device_uuid = env.sender().device_uuid();
    meta.from_display_name = env.sender().display_name();
    meta.file_name = fs_msg.file_name();
    meta.mime_type = fs_msg.mime_type();
    meta.total_size = fs_msg.total_size_bytes();
    file_transfer_meta_[fs_msg.transfer_id()] = meta;
  }

  void handle_file_chunk(const remboard::v1::Envelope& env) {
    const auto& fc = env.file_chunk();
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = file_transfers_.find(fc.transfer_id());
    if (it == file_transfers_.end()) return;  // unknown/expired transfer
    const std::string& data = fc.data();
    it->second->write_chunk(fc.index(),
                             std::vector<uint8_t>(data.begin(), data.end()));

    if (on_transfer_progress_) {
      TransferProgress p;
      p.transfer_id = fc.transfer_id();
      p.peer_device_uuid = env.sender().device_uuid();
      p.direction = TransferDirection::kIncoming;
      p.chunks_done = it->second->chunks_received();
      p.total_chunks = it->second->total_chunks();
      auto meta_it = file_transfer_meta_.find(fc.transfer_id());
      if (meta_it != file_transfer_meta_.end())
        p.file_name = meta_it->second.file_name;
      on_transfer_progress_(p);
    }
  }

  void handle_file_end(const remboard::v1::Envelope& env) {
    namespace fs = std::filesystem;
    const auto& end = env.file_end();

    std::unique_ptr<FileReceiveState> state;
    FileTransferMeta meta;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      auto it = file_transfers_.find(end.transfer_id());
      if (it == file_transfers_.end()) return;
      state = std::move(it->second);
      file_transfers_.erase(it);
      auto meta_it = file_transfer_meta_.find(end.transfer_id());
      if (meta_it != file_transfer_meta_.end()) {
        meta = meta_it->second;
        file_transfer_meta_.erase(meta_it);
      }
    }
    state->close();

    bool ok = state->all_received();
    std::string actual_checksum;
    if (ok) {
      actual_checksum = sha256_file_hex(state->dest_path());
      ok = actual_checksum == end.sha256_checksum();
    }

    if (!ok) {
      std::error_code ec;
      fs::remove(state->dest_path(), ec);
      if (auto dev = device_store_.find(meta.from_device_uuid)) {
        remboard::v1::Envelope reject_env;
        fill_envelope_header(reject_env);
        auto* rej = reject_env.mutable_reject();
        rej->set_reject_envelope_id(env.envelope_id());
        rej->set_reason("checksum_mismatch");
        send_envelope_to_device(*dev, reject_env);
      }
      return;
    }

    IncomingItem item;
    item.envelope_id = env.envelope_id();
    item.from_device_uuid = meta.from_device_uuid;
    item.from_display_name = meta.from_display_name;
    item.kind = ItemKind::kFile;
    item.received_at_unix_ms = now_unix_ms();
    item.file_name = meta.file_name;
    item.file_path = state->dest_path();
    item.mime_type = meta.mime_type;
    item.file_size_bytes = meta.total_size;
    deliver_item(item);
  }

  void deliver_item(const IncomingItem& item) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      received_items_[item.envelope_id] = item;
    }
    if (on_incoming_item_) on_incoming_item_(item);
  }

  struct FileTransferMeta {
    std::string from_device_uuid;
    std::string from_display_name;
    std::string file_name;
    std::string mime_type;
    int64_t total_size = 0;
  };

  PlatformHooks hooks_;
  Identity identity_;
  DeviceStore device_store_;
  ZmqTransport transport_;

  std::mutex state_mutex_;
  std::atomic<bool> pairing_mode_active_{false};
  int64_t pairing_started_at_ms_ = 0;
  std::string current_pairing_nonce_;
  std::unordered_map<std::string, PendingPairingInfo> pending_pairings_;
  std::unordered_map<std::string, std::pair<std::string, uint16_t>>
      outgoing_pairing_endpoints_;
  std::unordered_map<std::string, bool> online_status_;
  std::unordered_map<std::string, IncomingItem> received_items_;
  std::unordered_map<std::string, std::unique_ptr<FileReceiveState>>
      file_transfers_;
  std::unordered_map<std::string, FileTransferMeta> file_transfer_meta_;

  std::function<void(IncomingItem)> on_incoming_item_;
  std::function<void(PairingRequest)> on_pairing_request_;
  std::function<void(TransferProgress)> on_transfer_progress_;
  std::function<void(DeviceInfo)> on_peer_status_changed_;
};

std::unique_ptr<Core> Core::create(PlatformHooks hooks) {
  return std::make_unique<CoreImpl>(std::move(hooks));
}

}  // namespace remboard
