package dev.miklo.remboard.jni

/**
 * Thin JNI boundary to the shared C++23 core (see core/include/remboard/core.h).
 * All calls are request/response JSON strings, mirroring the app-linux
 * webview bridge -- the same wire shapes are reused so both platforms'
 * glue code stays easy to compare.
 *
 * start()/stop() own the native Core's lifecycle and are only ever called
 * from RemboardForegroundService, which is the sole owner. Everything else
 * (MainActivity, ShareReceiverActivity) just calls the query/action
 * methods, assuming the service is already running (they ensure that via
 * startForegroundService before touching this object).
 */
object RemboardNative {
    init {
        System.loadLibrary("remboard_jni")
    }

    external fun start(dataDir: String, displayName: String, listenPort: Int)
    external fun stop()

    external fun selfInfo(): String
    external fun listDevices(): String
    external fun removeDevice(deviceUuid: String)

    external fun requestPairing(qrPayloadJson: String): String

    external fun sendText(deviceUuid: String, text: String, sourceApp: String): String
    external fun sendFile(deviceUuid: String, filePath: String): String

    external fun actOnItem(envelopeId: String, action: String, saveToPath: String)

    // Called by NsdDiscovery once it resolves a paired peer's current
    // address, forwarding into Core's IDiscovery callback.
    external fun onPeerResolved(deviceUuid: String, ip: String, port: Int)

    // Called by NsdDiscovery when a previously-resolved peer's
    // advertisement disappears (it left the network, or its app closed).
    external fun onPeerLost(deviceUuid: String)

    interface Listener {
        fun onIncomingItem(itemJson: String)
        fun onTransferProgress(progressJson: String)
        fun onPeerStatusChanged(deviceJson: String)
    }

    private val listeners = mutableListOf<Listener>()

    @Synchronized
    fun addListener(listener: Listener) {
        listeners.add(listener)
    }

    @Synchronized
    fun removeListener(listener: Listener) {
        listeners.remove(listener)
    }

    // Invoked from native code (core's background thread, attached to the
    // JVM by the JNI bridge) -- fan out to all registered listeners.
    @JvmStatic
    @Synchronized
    fun dispatchIncomingItem(itemJson: String) {
        listeners.forEach { it.onIncomingItem(itemJson) }
    }

    @JvmStatic
    @Synchronized
    fun dispatchTransferProgress(progressJson: String) {
        listeners.forEach { it.onTransferProgress(progressJson) }
    }

    @JvmStatic
    @Synchronized
    fun dispatchPeerStatusChanged(deviceJson: String) {
        listeners.forEach { it.onPeerStatusChanged(deviceJson) }
    }
}
