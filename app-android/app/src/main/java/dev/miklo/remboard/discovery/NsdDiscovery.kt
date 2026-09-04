package dev.miklo.remboard.discovery

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import dev.miklo.remboard.jni.RemboardNative

/**
 * Android side of core's IDiscovery (core/include/remboard/idiscovery.h):
 * advertises this device as _remboard._tcp with its identity in TXT
 * records, and browses for other instances, reporting resolved paired
 * peers back into native via RemboardNative.onPeerResolved. Native code
 * (core/src/discovery/discovery_jni_shim.cpp) calls startAdvertising()
 * once at Core startup; there's no separate "resolve one peer" entry point
 * since we just forward every resolved instance and let Core filter to
 * peers it actually trusts.
 */
object NsdDiscovery {
    private const val SERVICE_TYPE = "_remboard._tcp."

    private lateinit var nsdManager: NsdManager
    private var registrationListener: NsdManager.RegistrationListener? = null
    private var discoveryListener: NsdManager.DiscoveryListener? = null
    private var selfDeviceUuid: String = ""

    // NsdManager's onServiceLost only gives back the service name, not the
    // TXT records -- remember what we resolved it to so we can still
    // report which device_uuid went offline.
    private val serviceNameToUuid = mutableMapOf<String, String>()

    @JvmStatic
    fun init(context: Context) {
        nsdManager = context.applicationContext.getSystemService(Context.NSD_SERVICE) as NsdManager
    }

    // @JvmStatic: called via JNI's GetStaticMethodID (see jni_bridge.cpp),
    // which requires a true JVM static method, not Kotlin's default
    // instance-on-singleton dispatch.
    @JvmStatic
    fun startAdvertising(deviceUuid: String, pubkeyFingerprint: String, platform: String, port: Int) {
        selfDeviceUuid = deviceUuid

        val serviceInfo = NsdServiceInfo().apply {
            serviceName = "remboard-" + deviceUuid.take(8)
            serviceType = SERVICE_TYPE
            setPort(port)
            setAttribute("device_uuid", deviceUuid)
            setAttribute("pubkey_fp", pubkeyFingerprint)
            setAttribute("platform", platform)
            setAttribute("proto", "1")
        }

        registrationListener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) {}
            override fun onRegistrationFailed(info: NsdServiceInfo, errorCode: Int) {}
            override fun onServiceUnregistered(info: NsdServiceInfo) {}
            override fun onUnregistrationFailed(info: NsdServiceInfo, errorCode: Int) {}
        }
        nsdManager.registerService(serviceInfo, NsdManager.PROTOCOL_DNS_SD, registrationListener)

        discoveryListener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {}

            override fun onServiceFound(service: NsdServiceInfo) {
                nsdManager.resolveService(service, object : NsdManager.ResolveListener {
                    override fun onResolveFailed(info: NsdServiceInfo, errorCode: Int) {}
                    override fun onServiceResolved(info: NsdServiceInfo) {
                        val uuidBytes = info.attributes["device_uuid"] ?: return
                        val uuid = String(uuidBytes)
                        if (uuid.isEmpty() || uuid == selfDeviceUuid) return
                        val ip = info.host?.hostAddress ?: return
                        serviceNameToUuid[info.serviceName] = uuid
                        RemboardNative.onPeerResolved(uuid, ip, info.port)
                    }
                })
            }

            override fun onServiceLost(service: NsdServiceInfo) {
                val uuid = serviceNameToUuid.remove(service.serviceName) ?: return
                RemboardNative.onPeerLost(uuid)
            }
            override fun onDiscoveryStopped(serviceType: String) {}

            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                runCatching { nsdManager.stopServiceDiscovery(this) }
            }

            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
        }
        nsdManager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discoveryListener)
    }

    @JvmStatic
    fun stop() {
        registrationListener?.let { runCatching { nsdManager.unregisterService(it) } }
        discoveryListener?.let { runCatching { nsdManager.stopServiceDiscovery(it) } }
        registrationListener = null
        discoveryListener = null
        serviceNameToUuid.clear()
    }
}
