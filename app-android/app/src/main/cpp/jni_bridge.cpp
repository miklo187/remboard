// The only translation unit in this app that includes jni.h. Owns the
// single global Core instance for the process's lifetime (created once by
// RemboardForegroundService, which is the sole caller of start()/stop()),
// and translates between JNI calls and Core's plain-C++ API. JSON shapes
// mirror app-linux/src/bridge.cpp so both platforms' glue code stay easy
// to compare.

#include <jni.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "remboard/core.h"
#include "remboard/isecret_store.h"
#include "remboard/platform/discovery_jni.h"

namespace {

JavaVM* g_jvm = nullptr;

// env->FindClass() only resolves app classes correctly when called from a
// thread that was started by the JVM (it walks that thread's classloader).
// Core's own background ZMQ thread attaches itself via
// AttachCurrentThreadAsDaemon, which gets the *system* classloader --
// FindClass("dev/miklo/...") from there throws ClassNotFoundException and
// crashes the process. So every class we call into from that thread is
// resolved once via JNI_OnLoad (which does run on a JVM-started thread) and
// kept as a global ref here instead of being looked up per call.
jclass g_cls_encrypted_device_store = nullptr;
jclass g_cls_nsd_discovery = nullptr;
jclass g_cls_remboard_native = nullptr;

// Any thread may need to call back into Java: Core's own background ZMQ
// thread (event callbacks, IDiscovery/ISecretStore access) is never a
// JNI-created thread, so it must attach itself first.
class JniEnvGuard {
 public:
  JniEnvGuard() {
    if (g_jvm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) ==
        JNI_EDETACHED) {
      g_jvm->AttachCurrentThreadAsDaemon(&env_, nullptr);
      attached_ = true;
    }
  }
  ~JniEnvGuard() {
    if (attached_) g_jvm->DetachCurrentThread();
  }
  JNIEnv* env() const { return env_; }

 private:
  JNIEnv* env_ = nullptr;
  bool attached_ = false;
};

std::string jstring_to_string(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* chars = env->GetStringUTFChars(s, nullptr);
  std::string result(chars);
  env->ReleaseStringUTFChars(s, chars);
  return result;
}

jstring to_jstring(JNIEnv* env, const std::string& s) {
  return env->NewStringUTF(s.c_str());
}

void call_static_void_with_string(jclass cls, const char* method_name,
                                   const std::string& json) {
  if (!cls) return;
  JniEnvGuard guard;
  JNIEnv* env = guard.env();
  jmethodID mid = env->GetStaticMethodID(cls, method_name,
                                          "(Ljava/lang/String;)V");
  if (mid) {
    jstring j_json = to_jstring(env, json);
    env->CallStaticVoidMethod(cls, mid, j_json);
    env->DeleteLocalRef(j_json);
  }
}

// ---- JSON shapes, mirroring app-linux/src/bridge.cpp ----

nlohmann::json device_to_json(const remboard::DeviceInfo& d) {
  nlohmann::json j;
  j["device_uuid"] = d.device_uuid;
  j["display_name"] = d.display_name;
  j["platform"] = remboard::to_string(d.platform);
  j["online"] = d.online;
  j["last_known_ip"] = d.last_known_ip;
  j["last_known_port"] = d.last_known_port;
  return j;
}

nlohmann::json item_to_json(const remboard::IncomingItem& item) {
  nlohmann::json j;
  j["envelope_id"] = item.envelope_id;
  j["from_device_uuid"] = item.from_device_uuid;
  j["from_display_name"] = item.from_display_name;
  switch (item.kind) {
    case remboard::ItemKind::kFile:
      j["kind"] = "file";
      break;
    case remboard::ItemKind::kClipboardText:
      j["kind"] = "clipboard";
      break;
    case remboard::ItemKind::kText:
    default:
      j["kind"] = "text";
      break;
  }
  j["received_at_unix_ms"] = item.received_at_unix_ms;
  j["text"] = item.text;
  j["source_app"] = item.source_app;
  j["file_name"] = item.file_name;
  j["mime_type"] = item.mime_type;
  j["file_size_bytes"] = item.file_size_bytes;
  return j;
}

nlohmann::json progress_to_json(const remboard::TransferProgress& p) {
  nlohmann::json j;
  j["transfer_id"] = p.transfer_id;
  j["peer_device_uuid"] = p.peer_device_uuid;
  j["file_name"] = p.file_name;
  j["direction"] = p.direction == remboard::TransferDirection::kIncoming
                        ? "incoming"
                        : "outgoing";
  j["chunks_done"] = p.chunks_done;
  j["total_chunks"] = p.total_chunks;
  return j;
}

// ---- ISecretStore backed by Kotlin's EncryptedDeviceStore ----

class JniSecretStore : public remboard::ISecretStore {
 public:
  std::optional<std::string> load(const std::string& key) override {
    if (!g_cls_encrypted_device_store) return std::nullopt;
    JniEnvGuard guard;
    JNIEnv* env = guard.env();
    jmethodID mid = env->GetStaticMethodID(
        g_cls_encrypted_device_store, "load",
        "(Ljava/lang/String;)Ljava/lang/String;");
    jstring j_key = to_jstring(env, key);
    auto j_result = static_cast<jstring>(
        env->CallStaticObjectMethod(g_cls_encrypted_device_store, mid, j_key));
    env->DeleteLocalRef(j_key);
    if (!j_result) return std::nullopt;
    std::string result = jstring_to_string(env, j_result);
    env->DeleteLocalRef(j_result);
    return result;
  }

  void save(const std::string& key, const std::string& value) override {
    if (!g_cls_encrypted_device_store) return;
    JniEnvGuard guard;
    JNIEnv* env = guard.env();
    jmethodID mid = env->GetStaticMethodID(
        g_cls_encrypted_device_store, "save",
        "(Ljava/lang/String;Ljava/lang/String;)V");
    jstring j_key = to_jstring(env, key);
    jstring j_value = to_jstring(env, value);
    env->CallStaticVoidMethod(g_cls_encrypted_device_store, mid, j_key, j_value);
    env->DeleteLocalRef(j_key);
    env->DeleteLocalRef(j_value);
  }
};

JniSecretStore g_secret_store;
remboard::AndroidDiscovery g_discovery;
std::unique_ptr<remboard::Core> g_core;
std::mutex g_core_mutex;

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  g_jvm = vm;

  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    return JNI_VERSION_1_6;

  auto cache_class = [&](const char* name) -> jclass {
    jclass local = env->FindClass(name);
    if (!local) return nullptr;
    auto global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
  };
  g_cls_encrypted_device_store =
      cache_class("dev/miklo/remboard/storage/EncryptedDeviceStore");
  g_cls_nsd_discovery = cache_class("dev/miklo/remboard/discovery/NsdDiscovery");
  g_cls_remboard_native = cache_class("dev/miklo/remboard/jni/RemboardNative");

  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL Java_dev_miklo_remboard_jni_RemboardNative_start(
    JNIEnv* env, jobject, jstring data_dir, jstring display_name,
    jint listen_port) {
  std::lock_guard<std::mutex> lock(g_core_mutex);
  if (g_core) return;

  g_discovery.set_advertise_hook([](const std::string& uuid,
                                     const std::string& fp,
                                     const std::string& platform,
                                     uint16_t port) {
    if (!g_cls_nsd_discovery) return;
    JniEnvGuard guard;
    JNIEnv* env = guard.env();
    jmethodID mid = env->GetStaticMethodID(
        g_cls_nsd_discovery, "startAdvertising",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");
    if (mid) {
      jstring j_uuid = to_jstring(env, uuid);
      jstring j_fp = to_jstring(env, fp);
      jstring j_platform = to_jstring(env, platform);
      env->CallStaticVoidMethod(g_cls_nsd_discovery, mid, j_uuid, j_fp,
                                 j_platform, static_cast<jint>(port));
      env->DeleteLocalRef(j_uuid);
      env->DeleteLocalRef(j_fp);
      env->DeleteLocalRef(j_platform);
    }
  });
  g_discovery.set_stop_hook([]() {
    if (!g_cls_nsd_discovery) return;
    JniEnvGuard guard;
    JNIEnv* env = guard.env();
    jmethodID mid = env->GetStaticMethodID(g_cls_nsd_discovery, "stop", "()V");
    if (mid) env->CallStaticVoidMethod(g_cls_nsd_discovery, mid);
  });

  remboard::PlatformHooks hooks;
  hooks.secret_store = &g_secret_store;
  hooks.discovery = &g_discovery;
  hooks.data_dir = jstring_to_string(env, data_dir);
  hooks.display_name = jstring_to_string(env, display_name);
  hooks.platform = remboard::Platform::kAndroid;
  hooks.listen_port = static_cast<uint16_t>(listen_port);

  g_core = remboard::Core::create(hooks);

  g_core->set_on_incoming_item([](remboard::IncomingItem item) {
    call_static_void_with_string(g_cls_remboard_native, "dispatchIncomingItem",
                                  item_to_json(item).dump());
  });
  g_core->set_on_transfer_progress([](remboard::TransferProgress p) {
    call_static_void_with_string(g_cls_remboard_native,
                                  "dispatchTransferProgress",
                                  progress_to_json(p).dump());
  });
  g_core->set_on_peer_status_changed([](remboard::DeviceInfo d) {
    call_static_void_with_string(g_cls_remboard_native,
                                  "dispatchPeerStatusChanged",
                                  device_to_json(d).dump());
  });

  g_core->start();
}

JNIEXPORT void JNICALL
Java_dev_miklo_remboard_jni_RemboardNative_stop(JNIEnv*, jobject) {
  std::lock_guard<std::mutex> lock(g_core_mutex);
  if (g_core) g_core->shutdown();
}

JNIEXPORT jstring JNICALL
Java_dev_miklo_remboard_jni_RemboardNative_selfInfo(JNIEnv* env, jobject) {
  if (!g_core) return to_jstring(env, "{}");
  return to_jstring(env, device_to_json(g_core->self_info()).dump());
}

JNIEXPORT jstring JNICALL
Java_dev_miklo_remboard_jni_RemboardNative_listDevices(JNIEnv* env, jobject) {
  if (!g_core) return to_jstring(env, "[]");
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& d : g_core->list_paired_devices())
    arr.push_back(device_to_json(d));
  return to_jstring(env, arr.dump());
}

JNIEXPORT void JNICALL Java_dev_miklo_remboard_jni_RemboardNative_removeDevice(
    JNIEnv* env, jobject, jstring device_uuid) {
  if (g_core) g_core->remove_device(jstring_to_string(env, device_uuid));
}

JNIEXPORT jstring JNICALL
Java_dev_miklo_remboard_jni_RemboardNative_requestPairing(
    JNIEnv* env, jobject, jstring qr_payload_json) {
  if (!g_core)
    return to_jstring(env, R"({"ok":false,"error":"core not started"})");
  try {
    g_core->request_pairing(jstring_to_string(env, qr_payload_json));
    return to_jstring(env, R"({"ok":true})");
  } catch (const std::exception& e) {
    nlohmann::json j;
    j["ok"] = false;
    j["error"] = e.what();
    return to_jstring(env, j.dump());
  }
}

JNIEXPORT jstring JNICALL Java_dev_miklo_remboard_jni_RemboardNative_sendText(
    JNIEnv* env, jobject, jstring device_uuid, jstring text,
    jstring source_app) {
  if (!g_core)
    return to_jstring(env, R"({"ok":false,"error":"core not started"})");
  try {
    std::string id =
        g_core->send_text(jstring_to_string(env, device_uuid),
                           jstring_to_string(env, text),
                           jstring_to_string(env, source_app));
    nlohmann::json j;
    j["ok"] = true;
    j["envelope_id"] = id;
    return to_jstring(env, j.dump());
  } catch (const std::exception& e) {
    nlohmann::json j;
    j["ok"] = false;
    j["error"] = e.what();
    return to_jstring(env, j.dump());
  }
}

JNIEXPORT jstring JNICALL Java_dev_miklo_remboard_jni_RemboardNative_sendFile(
    JNIEnv* env, jobject, jstring device_uuid, jstring file_path) {
  if (!g_core)
    return to_jstring(env, R"({"ok":false,"error":"core not started"})");
  try {
    std::string id = g_core->send_file(jstring_to_string(env, device_uuid),
                                        jstring_to_string(env, file_path));
    nlohmann::json j;
    j["ok"] = true;
    j["transfer_id"] = id;
    return to_jstring(env, j.dump());
  } catch (const std::exception& e) {
    nlohmann::json j;
    j["ok"] = false;
    j["error"] = e.what();
    return to_jstring(env, j.dump());
  }
}

JNIEXPORT void JNICALL Java_dev_miklo_remboard_jni_RemboardNative_actOnItem(
    JNIEnv* env, jobject, jstring envelope_id, jstring action,
    jstring save_to_path) {
  if (!g_core) return;
  std::string action_str = jstring_to_string(env, action);
  remboard::ItemAction a = remboard::ItemAction::kCopy;
  if (action_str == "reject") a = remboard::ItemAction::kReject;
  else if (action_str == "save") a = remboard::ItemAction::kSave;
  else if (action_str == "dismiss") a = remboard::ItemAction::kDismiss;
  g_core->act_on_item(jstring_to_string(env, envelope_id), a,
                       jstring_to_string(env, save_to_path));
}

JNIEXPORT void JNICALL
Java_dev_miklo_remboard_jni_RemboardNative_onPeerResolved(
    JNIEnv* env, jobject, jstring device_uuid, jstring ip, jint port) {
  g_discovery.notify_peer_resolved(jstring_to_string(env, device_uuid),
                                    jstring_to_string(env, ip),
                                    static_cast<uint16_t>(port));
}

JNIEXPORT void JNICALL
Java_dev_miklo_remboard_jni_RemboardNative_onPeerLost(JNIEnv* env, jobject,
                                                        jstring device_uuid) {
  g_discovery.notify_peer_lost(jstring_to_string(env, device_uuid));
}

}  // extern "C"
