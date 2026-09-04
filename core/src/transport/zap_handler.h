#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include <zmq.hpp>

namespace remboard {

// Runs libzmq's ZAP (RFC 27, https://rfc.zeromq.org/spec/27/) authentication
// handler on its own thread, bound to inproc://zeromq.zap.01 in the given
// context. Must be started before any CURVE-server socket in the same
// context (with ZMQ_ZAP_DOMAIN set) accepts connections, since ZAP checks
// are only invoked when a ZAP domain is configured on the socket.
class ZapHandler {
 public:
  // Called with the connecting client's raw 32-byte CURVE public key;
  // returns true to accept the connection.
  using IsAllowedFn = std::function<bool(const std::vector<uint8_t>&)>;

  ZapHandler(zmq::context_t& context, IsAllowedFn is_allowed);
  ~ZapHandler();

  void start();
  void stop();

 private:
  void run();

  zmq::context_t& context_;
  IsAllowedFn is_allowed_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace remboard
