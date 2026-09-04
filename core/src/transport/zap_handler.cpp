#include "transport/zap_handler.h"

#include <chrono>
#include <iostream>

namespace remboard {

ZapHandler::ZapHandler(zmq::context_t& context, IsAllowedFn is_allowed)
    : context_(context), is_allowed_(std::move(is_allowed)) {}

ZapHandler::~ZapHandler() { stop(); }

void ZapHandler::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread(&ZapHandler::run, this);
}

void ZapHandler::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

void ZapHandler::run() {
  zmq::socket_t handler(context_, zmq::socket_type::router);
  handler.set(zmq::sockopt::rcvtimeo, 200);
  handler.bind("inproc://zeromq.zap.01");

  while (running_.load()) {
    std::vector<zmq::message_t> frames;
    zmq::message_t part;
    bool got_any = false;
    bool more = true;
    while (more) {
      auto result = handler.recv(part, zmq::recv_flags::none);
      if (!result.has_value()) {
        // Timed out (rcvtimeo) with nothing received yet.
        more = false;
        break;
      }
      got_any = true;
      more = part.more();
      frames.push_back(std::move(part));
    }
    if (!got_any) continue;
    if (frames.size() < 9) continue;

    // The internal libzmq socket connecting to inproc://zeromq.zap.01
    // follows REQ-style framing (an empty delimiter after the routing
    // envelope), so the ZAP body starts one frame later than RFC 27's
    // frame list alone would suggest:
    // [envelope, "", version, request_id, domain, address, identity,
    //  mechanism, credentials...]
    const zmq::message_t& envelope = frames[0];
    const zmq::message_t& version = frames[2];
    const zmq::message_t& request_id = frames[3];
    const zmq::message_t& mechanism = frames[7];

    std::string mechanism_str(static_cast<const char*>(mechanism.data()),
                               mechanism.size());

    bool allowed = false;
    std::string user_id;
    if (mechanism_str == "CURVE" && frames.size() >= 9) {
      const zmq::message_t& credentials = frames[8];
      std::vector<uint8_t> pubkey(
          static_cast<const uint8_t*>(credentials.data()),
          static_cast<const uint8_t*>(credentials.data()) +
              credentials.size());
      allowed = is_allowed_(pubkey);
    }

    std::string status_code = allowed ? "200" : "400";
    std::string status_text = allowed ? "OK" : "Denied";

    handler.send(zmq::message_t(envelope.data(), envelope.size()),
                 zmq::send_flags::sndmore);
    handler.send(zmq::message_t(), zmq::send_flags::sndmore);  // delimiter
    handler.send(zmq::message_t(version.data(), version.size()),
                 zmq::send_flags::sndmore);
    handler.send(zmq::message_t(request_id.data(), request_id.size()),
                 zmq::send_flags::sndmore);
    handler.send(zmq::message_t(status_code), zmq::send_flags::sndmore);
    handler.send(zmq::message_t(status_text), zmq::send_flags::sndmore);
    handler.send(zmq::message_t(user_id), zmq::send_flags::sndmore);
    handler.send(zmq::message_t(), zmq::send_flags::none);  // metadata
  }
}

}  // namespace remboard
