# remboard

Send text and files between your phone and PC over the local network — end-to-end encrypted, no cloud, no account.

## Features

- **Text and files, both directions** — share to remboard from any Android app, paste into the desktop app, or compose from either side.
- **End-to-end encrypted** — every connection uses [CurveZMQ](http://curvezmq.org/) (Curve25519 via libsodium); only paired devices are accepted.
- **QR code pairing** — scan a code shown on the PC to pair, no typing keys by hand.
- **LAN discovery** — devices find each other automatically via mDNS/Avahi once paired.
- **No servers, no accounts** — everything travels directly between your devices on the same network.

## How it works

```
   phone (Android)                      PC (Linux)
  ┌─────────────────┐                 ┌─────────────────┐
  │  app-android/    │  CurveZMQ/TCP   │  app-linux/      │
  │  (Kotlin + JNI)  │◄───────────────►│  (webview UI)    │
  └────────┬─────────┘   (encrypted)   └────────┬─────────┘
           │                                     │
           └──────────────┬──────────────────────┘
                           │
                    core/ (C++23)
              ZeroMQ transport, pairing,
              device registry, file chunking
                           │
                     proto/remboard.proto
                    (message definitions)
```

Both apps share the same C++ core (`core/`), which owns the transport, pairing, and device state; each platform only implements its own UI and OS glue (`app-android/`, `app-linux/`).

Pairing: the PC shows a QR code containing its Curve25519 public key, IP, and port. The phone scans it to establish trust out of band — device keys are never trusted blindly over the network. Once paired, devices discover each other locally over mDNS and exchange messages over a CURVE-encrypted ZeroMQ ROUTER/DEALER connection; unpaired peers are rejected via ZAP.

## Building

### Linux desktop app

Requires CMake 3.19+, a C++23 compiler, and GTK3/WebKitGTK, ZeroMQ, libsodium, and Protobuf development packages (see `core/CMakeLists.txt` and `app-linux/CMakeLists.txt` for exact dependencies).

```sh
cmake -B build
cmake --build build
./build/app-linux/remboard
```

### Android app

Requires the Android SDK/NDK (see `app-android/local.properties.example`).

```sh
cd app-android
cp local.properties.example local.properties   # then set sdk.dir
./gradlew installDebug
```

The native core is built for the device automatically via `externalNativeBuild` (see `app-android/app/src/main/cpp/CMakeLists.txt`).

### Tests

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

## Project layout

| Path | What |
|---|---|
| `core/` | Shared C++23 core: ZeroMQ/CURVE transport, pairing, device registry, file chunking |
| `proto/` | Protobuf message definitions shared by all platforms |
| `app-android/` | Android app (Kotlin, JNI bridge into `core/`) |
| `app-linux/` | Linux desktop app (webview-based UI over the C++ core) |

## License

MIT — see [LICENSE](LICENSE).
