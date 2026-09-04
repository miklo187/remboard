#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <zmq.hpp>

#include "remboard/types.h"
#include "transport/zap_handler.h"

namespace remboard {

// Owns the ZeroMQ context, the CURVE-server ROUTER socket that receives all
// inbound traffic, one CURVE-client DEALER socket per peer used for
// outbound sends, and the ZAP authentication handler. All ZMQ socket
// access happens on a single internal IO thread; public methods hand work
// to it via a small command queue so callers on any thread (UI, JNI) can
// call them safely.
class ZmqTransport {
 public:
  // sender_device_uuid: the peer's routing id (== their device_uuid, since
  //   outbound DEALERs set ZMQ_ROUTING_ID to their own device_uuid).
  // peer_ip: best-effort source IP of the connection (from ZMQ's
  //   "Peer-Address" message metadata), empty if unavailable.
  using MessageCallback = std::function<void(
      const std::string& sender_device_uuid, const std::string& peer_ip,
      const std::vector<uint8_t>& payload)>;

  ZmqTransport(PubKey own_public_key, std::vector<uint8_t> own_secret_key,
               uint16_t listen_port, ZapHandler::IsAllowedFn zap_is_allowed);
  ~ZmqTransport();

  void start();
  void stop();

  void set_on_message(MessageCallback callback);

  // Sends payload to a peer, creating (or reusing, or re-creating if the
  // endpoint changed) a cached CURVE-client DEALER socket connected to
  // tcp://ip:port with ZMQ_CURVE_SERVERKEY = peer_pubkey and
  // ZMQ_ROUTING_ID = own_device_uuid. Async: queues onto the IO thread and
  // returns immediately.
  void send_to_peer(const std::string& own_device_uuid,
                     const std::string& peer_device_uuid,
                     const PubKey& peer_pubkey, const std::string& ip,
                     uint16_t port, std::vector<uint8_t> payload);

  // Replies directly to a routing id previously seen on the ROUTER socket
  // (used for the pairing PairResponse round-trip, avoiding a second
  // outbound connection). Async, same as send_to_peer.
  void reply_via_router(const std::string& routing_id,
                         std::vector<uint8_t> payload);

 private:
  struct PeerDealer {
    zmq::socket_t socket;
    std::string endpoint;
  };
  struct SendCommand {
    std::string own_device_uuid;
    std::string peer_device_uuid;
    PubKey peer_pubkey;
    std::string ip;
    uint16_t port;
    std::vector<uint8_t> payload;
  };
  struct ReplyCommand {
    std::string routing_id;
    std::vector<uint8_t> payload;
  };
  using Command = std::variant<SendCommand, ReplyCommand>;

  void io_loop();
  void handle_command(const Command& cmd);
  void drain_router();
  void drain_dealer(const std::string& peer_device_uuid,
                     zmq::socket_t& dealer);
  zmq::socket_t& get_or_create_dealer(const std::string& own_device_uuid,
                                       const std::string& peer_device_uuid,
                                       const PubKey& peer_pubkey,
                                       const std::string& ip, uint16_t port);

  PubKey own_public_key_;
  std::vector<uint8_t> own_secret_key_;
  uint16_t listen_port_;

  zmq::context_t context_;
  zmq::socket_t router_;
  std::unique_ptr<ZapHandler> zap_handler_;
  ZapHandler::IsAllowedFn zap_is_allowed_;
  std::unordered_map<std::string, PeerDealer> dealers_;

  std::thread io_thread_;
  std::atomic<bool> running_{false};

  std::mutex cmd_mutex_;
  std::deque<Command> commands_;

  MessageCallback on_message_;
};

}  // namespace remboard
