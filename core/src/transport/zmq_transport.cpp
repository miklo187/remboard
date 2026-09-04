#include "transport/zmq_transport.h"

namespace remboard {

namespace {
std::string bytes_to_str(const std::vector<uint8_t>& b) {
  return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}
}  // namespace

ZmqTransport::ZmqTransport(PubKey own_public_key,
                            std::vector<uint8_t> own_secret_key,
                            uint16_t listen_port,
                            ZapHandler::IsAllowedFn zap_is_allowed)
    : own_public_key_(std::move(own_public_key)),
      own_secret_key_(std::move(own_secret_key)),
      listen_port_(listen_port),
      context_(1),
      router_(context_, zmq::socket_type::router),
      zap_is_allowed_(std::move(zap_is_allowed)) {}

ZmqTransport::~ZmqTransport() { stop(); }

void ZmqTransport::start() {
  if (running_.exchange(true)) return;

  // ZAP handler must be listening before the ROUTER accepts connections
  // that require authentication.
  zap_handler_ = std::make_unique<ZapHandler>(context_, zap_is_allowed_);
  zap_handler_->start();

  router_.set(zmq::sockopt::curve_server, 1);
  router_.set(zmq::sockopt::curve_secretkey, bytes_to_str(own_secret_key_));
  router_.set(zmq::sockopt::curve_publickey, bytes_to_str(own_public_key_));
  router_.set(zmq::sockopt::zap_domain, std::string("remboard"));
  router_.bind("tcp://*:" + std::to_string(listen_port_));

  io_thread_ = std::thread(&ZmqTransport::io_loop, this);
}

void ZmqTransport::stop() {
  if (!running_.exchange(false)) return;
  if (io_thread_.joinable()) io_thread_.join();
  dealers_.clear();
  if (zap_handler_) zap_handler_->stop();
}

void ZmqTransport::set_on_message(MessageCallback callback) {
  on_message_ = std::move(callback);
}

void ZmqTransport::send_to_peer(const std::string& own_device_uuid,
                                 const std::string& peer_device_uuid,
                                 const PubKey& peer_pubkey,
                                 const std::string& ip, uint16_t port,
                                 std::vector<uint8_t> payload) {
  std::lock_guard<std::mutex> lock(cmd_mutex_);
  commands_.push_back(SendCommand{own_device_uuid, peer_device_uuid,
                                   peer_pubkey, ip, port,
                                   std::move(payload)});
}

void ZmqTransport::reply_via_router(const std::string& routing_id,
                                     std::vector<uint8_t> payload) {
  std::lock_guard<std::mutex> lock(cmd_mutex_);
  commands_.push_back(ReplyCommand{routing_id, std::move(payload)});
}

zmq::socket_t& ZmqTransport::get_or_create_dealer(
    const std::string& own_device_uuid, const std::string& peer_device_uuid,
    const PubKey& peer_pubkey, const std::string& ip, uint16_t port) {
  std::string endpoint = "tcp://" + ip + ":" + std::to_string(port);

  auto it = dealers_.find(peer_device_uuid);
  if (it != dealers_.end()) {
    if (it->second.endpoint == endpoint) return it->second.socket;
    dealers_.erase(it);
  }

  zmq::socket_t sock(context_, zmq::socket_type::dealer);
  sock.set(zmq::sockopt::routing_id, own_device_uuid);
  sock.set(zmq::sockopt::curve_secretkey, bytes_to_str(own_secret_key_));
  sock.set(zmq::sockopt::curve_publickey, bytes_to_str(own_public_key_));
  sock.set(zmq::sockopt::curve_serverkey, bytes_to_str(peer_pubkey));
  sock.connect(endpoint);

  auto [inserted_it, ok] = dealers_.emplace(
      peer_device_uuid, PeerDealer{std::move(sock), endpoint});
  return inserted_it->second.socket;
}

void ZmqTransport::handle_command(const Command& cmd) {
  if (const auto* send = std::get_if<SendCommand>(&cmd)) {
    zmq::socket_t& dealer =
        get_or_create_dealer(send->own_device_uuid, send->peer_device_uuid,
                              send->peer_pubkey, send->ip, send->port);
    dealer.send(
        zmq::message_t(send->payload.data(), send->payload.size()),
        zmq::send_flags::none);
  } else if (const auto* reply = std::get_if<ReplyCommand>(&cmd)) {
    router_.send(zmq::message_t(reply->routing_id.data(),
                                 reply->routing_id.size()),
                 zmq::send_flags::sndmore);
    router_.send(
        zmq::message_t(reply->payload.data(), reply->payload.size()),
        zmq::send_flags::none);
  }
}

namespace {
std::string extract_peer_ip(zmq::message_t& payload) {
  try {
    return payload.gets("Peer-Address");
  } catch (const zmq::error_t&) {
    return "";  // property unavailable
  }
}
}  // namespace

void ZmqTransport::drain_router() {
  while (true) {
    zmq::message_t routing_id;
    auto r1 = router_.recv(routing_id, zmq::recv_flags::dontwait);
    if (!r1.has_value()) break;
    if (!routing_id.more()) continue;  // malformed, drop

    zmq::message_t payload;
    auto r2 = router_.recv(payload, zmq::recv_flags::dontwait);
    if (!r2.has_value()) break;

    std::string sender_uuid(static_cast<const char*>(routing_id.data()),
                             routing_id.size());
    std::string peer_ip = extract_peer_ip(payload);

    if (on_message_) {
      std::vector<uint8_t> bytes(
          static_cast<const uint8_t*>(payload.data()),
          static_cast<const uint8_t*>(payload.data()) + payload.size());
      on_message_(sender_uuid, peer_ip, bytes);
    }
  }
}

void ZmqTransport::drain_dealer(const std::string& peer_device_uuid,
                                 zmq::socket_t& dealer) {
  while (true) {
    zmq::message_t payload;
    auto r = dealer.recv(payload, zmq::recv_flags::dontwait);
    if (!r.has_value()) break;
    // A well-formed reply from a ROUTER peer is a single frame; if more()
    // is somehow set, drain and ignore the extra frames defensively.
    while (payload.more()) {
      zmq::message_t extra;
      if (!dealer.recv(extra, zmq::recv_flags::dontwait).has_value()) break;
    }

    std::string peer_ip = extract_peer_ip(payload);
    if (on_message_) {
      std::vector<uint8_t> bytes(
          static_cast<const uint8_t*>(payload.data()),
          static_cast<const uint8_t*>(payload.data()) + payload.size());
      on_message_(peer_device_uuid, peer_ip, bytes);
    }
  }
}

void ZmqTransport::io_loop() {
  while (running_.load()) {
    // Drain queued commands first so any dealer created this tick is
    // included in this iteration's poll set below.
    {
      std::deque<Command> local;
      {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        local.swap(commands_);
      }
      for (const auto& cmd : local) handle_command(cmd);
    }

    std::vector<zmq::pollitem_t> items;
    items.push_back({router_.handle(), 0, ZMQ_POLLIN, 0});
    std::vector<std::string> dealer_uuids;
    dealer_uuids.reserve(dealers_.size());
    for (auto& [uuid, peer_dealer] : dealers_) {
      items.push_back({peer_dealer.socket.handle(), 0, ZMQ_POLLIN, 0});
      dealer_uuids.push_back(uuid);
    }

    zmq::poll(items.data(), items.size(), std::chrono::milliseconds(100));

    if (items[0].revents & ZMQ_POLLIN) drain_router();
    for (size_t i = 0; i < dealer_uuids.size(); ++i) {
      if (items[i + 1].revents & ZMQ_POLLIN) {
        drain_dealer(dealer_uuids[i], dealers_.at(dealer_uuids[i]).socket);
      }
    }
  }
}

}  // namespace remboard
