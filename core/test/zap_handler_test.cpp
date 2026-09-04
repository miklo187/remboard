// Integration tests driving real ZMQ CURVE handshakes against ZapHandler,
// exercising the actual RFC 27 frame layout libzmq's internal ZAP client
// uses (which has an extra empty delimiter frame that isn't obvious from
// the RFC text alone -- see the comment in zap_handler.cpp).

#include "transport/zap_handler.h"

#include <zmq.h>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include <zmq.hpp>

namespace {

struct CurveKeypair {
  std::vector<uint8_t> public_key;
  std::vector<uint8_t> secret_key;
};

CurveKeypair generate_keypair() {
  char z85_public[41];
  char z85_secret[41];
  if (zmq_curve_keypair(z85_public, z85_secret) != 0) {
    throw std::runtime_error("zmq_curve_keypair failed");
  }
  CurveKeypair kp;
  kp.public_key.resize(32);
  kp.secret_key.resize(32);
  zmq_z85_decode(kp.public_key.data(), z85_public);
  zmq_z85_decode(kp.secret_key.data(), z85_secret);
  return kp;
}

std::string bytes_to_str(const std::vector<uint8_t>& b) {
  return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

struct TestServer {
  zmq::context_t context{1};
  CurveKeypair server_kp = generate_keypair();
  std::unique_ptr<remboard::ZapHandler> handler;
  zmq::socket_t router;
  std::string endpoint;

  explicit TestServer(remboard::ZapHandler::IsAllowedFn is_allowed)
      : router(context, zmq::socket_type::router) {
    handler = std::make_unique<remboard::ZapHandler>(context, std::move(is_allowed));
    handler->start();

    // A rejected/never-delivered message must not block the test on
    // teardown: default LINGER is infinite, and zmq_close() would then
    // wait forever for a send that ZAP will never let through.
    router.set(zmq::sockopt::linger, 0);
    router.set(zmq::sockopt::curve_server, 1);
    router.set(zmq::sockopt::curve_secretkey, bytes_to_str(server_kp.secret_key));
    router.set(zmq::sockopt::curve_publickey, bytes_to_str(server_kp.public_key));
    router.set(zmq::sockopt::zap_domain, std::string("test"));
    router.bind("tcp://127.0.0.1:*");
    endpoint = router.get(zmq::sockopt::last_endpoint);
  }

  ~TestServer() { handler->stop(); }
};

zmq::socket_t make_client_dealer(zmq::context_t& context,
                                  const CurveKeypair& client_kp,
                                  const CurveKeypair& server_kp,
                                  const std::string& endpoint) {
  zmq::socket_t dealer(context, zmq::socket_type::dealer);
  dealer.set(zmq::sockopt::linger, 0);
  dealer.set(zmq::sockopt::routing_id, std::string("client-id"));
  dealer.set(zmq::sockopt::curve_secretkey, bytes_to_str(client_kp.secret_key));
  dealer.set(zmq::sockopt::curve_publickey, bytes_to_str(client_kp.public_key));
  dealer.set(zmq::sockopt::curve_serverkey, bytes_to_str(server_kp.public_key));
  dealer.connect(endpoint);
  return dealer;
}

}  // namespace

TEST(ZapHandler, AllowsConnectionWhenIsAllowedReturnsTrue) {
  TestServer server([](const std::vector<uint8_t>&) { return true; });
  auto client_kp = generate_keypair();
  zmq::socket_t dealer =
      make_client_dealer(server.context, client_kp, server.server_kp, server.endpoint);

  dealer.send(zmq::message_t(std::string("hello")), zmq::send_flags::none);

  zmq::pollitem_t items[] = {{server.router.handle(), 0, ZMQ_POLLIN, 0}};
  int rc = zmq::poll(items, 1, std::chrono::milliseconds(2000));
  ASSERT_GT(rc, 0) << "expected the message to arrive but ZAP blocked it";

  zmq::message_t routing_id, payload;
  ASSERT_TRUE(server.router.recv(routing_id, zmq::recv_flags::none).has_value());
  ASSERT_TRUE(server.router.recv(payload, zmq::recv_flags::none).has_value());
  EXPECT_EQ(payload.to_string(), "hello");
}

TEST(ZapHandler, RejectsConnectionWhenIsAllowedReturnsFalse) {
  TestServer server([](const std::vector<uint8_t>&) { return false; });
  auto client_kp = generate_keypair();
  zmq::socket_t dealer =
      make_client_dealer(server.context, client_kp, server.server_kp, server.endpoint);

  dealer.send(zmq::message_t(std::string("hello")), zmq::send_flags::none);

  zmq::pollitem_t items[] = {{server.router.handle(), 0, ZMQ_POLLIN, 0}};
  int rc = zmq::poll(items, 1, std::chrono::milliseconds(500));
  EXPECT_EQ(rc, 0) << "expected no message: ZAP should have rejected the handshake";
}

TEST(ZapHandler, IsAllowedReceivesTheConnectingClientsRealPublicKey) {
  std::vector<uint8_t> observed_key;
  TestServer server([&](const std::vector<uint8_t>& key) {
    observed_key = key;
    return true;
  });
  auto client_kp = generate_keypair();
  zmq::socket_t dealer =
      make_client_dealer(server.context, client_kp, server.server_kp, server.endpoint);

  dealer.send(zmq::message_t(std::string("hello")), zmq::send_flags::none);

  zmq::pollitem_t items[] = {{server.router.handle(), 0, ZMQ_POLLIN, 0}};
  ASSERT_GT(zmq::poll(items, 1, std::chrono::milliseconds(2000)), 0);
  zmq::message_t routing_id, payload;
  server.router.recv(routing_id, zmq::recv_flags::none);
  server.router.recv(payload, zmq::recv_flags::none);

  EXPECT_EQ(observed_key, client_kp.public_key);
}

TEST(ZapHandler, DistinctClientsGetIndependentAllowDecisions) {
  auto allowed_kp = generate_keypair();
  TestServer server([&](const std::vector<uint8_t>& key) {
    return key == allowed_kp.public_key;
  });

  // The allowed client's message should arrive...
  zmq::socket_t good_dealer =
      make_client_dealer(server.context, allowed_kp, server.server_kp, server.endpoint);
  good_dealer.send(zmq::message_t(std::string("from-good")), zmq::send_flags::none);

  zmq::pollitem_t items[] = {{server.router.handle(), 0, ZMQ_POLLIN, 0}};
  ASSERT_GT(zmq::poll(items, 1, std::chrono::milliseconds(2000)), 0);
  zmq::message_t routing_id, payload;
  server.router.recv(routing_id, zmq::recv_flags::none);
  server.router.recv(payload, zmq::recv_flags::none);
  EXPECT_EQ(payload.to_string(), "from-good");

  // ...but a second, untrusted client should be silently rejected.
  auto other_kp = generate_keypair();
  zmq::socket_t bad_dealer =
      make_client_dealer(server.context, other_kp, server.server_kp, server.endpoint);
  bad_dealer.send(zmq::message_t(std::string("from-bad")), zmq::send_flags::none);

  int rc = zmq::poll(items, 1, std::chrono::milliseconds(500));
  EXPECT_EQ(rc, 0);
}
