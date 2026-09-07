#pragma once

#include <string>

#include <webview/webview.h>

#include "remboard/core.h"

namespace remboard_app {

// Owns the JS<->C++ boundary: registers webview::bind() handlers that JS
// calls into (request/response, JSON-array args in, JSON out), and pushes
// Core events into the page via webview::dispatch()+eval() (JS callback
// functions named onIncomingItem/onPairingRequest/onTransferProgress/
// onPeerStatusChanged, defined in ui/app.js).
class Bridge {
 public:
  Bridge(remboard::Core& core, webview::webview& w);

  void register_handlers();
  void register_push_callbacks();

 private:
  std::string get_self_info(const std::string& req);
  std::string list_devices(const std::string& req);
  std::string begin_pairing(const std::string& req);
  std::string cancel_pairing(const std::string& req);
  std::string request_pairing(const std::string& req);
  std::string accept_pairing(const std::string& req);
  std::string reject_pairing(const std::string& req);
  std::string remove_device(const std::string& req);
  std::string send_text(const std::string& req);
  std::string pick_and_send_file(const std::string& req);
  std::string act_on_item(const std::string& req);
  std::string save_item(const std::string& req);

  remboard::Core& core_;
  webview::webview& w_;
};

}  // namespace remboard_app
