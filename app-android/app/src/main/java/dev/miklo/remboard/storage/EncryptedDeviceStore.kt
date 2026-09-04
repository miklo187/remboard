package dev.miklo.remboard.storage

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey

/**
 * Keystore-backed ISecretStore for the native core (see
 * core/include/remboard/isecret_store.h) -- holds this device's own
 * identity keypair and the trusted-device registry, both as small JSON
 * blobs under "identity"/"devices" keys. Called from native code via the
 * JNI bridge (core/src/discovery and jni_bridge.cpp), so every method here
 * must be safe to call from any thread.
 */
object EncryptedDeviceStore {
    private const val PREFS_NAME = "remboard_secrets"

    @Volatile
    private var prefs: SharedPreferences? = null

    fun init(context: Context) {
        if (prefs != null) return
        synchronized(this) {
            if (prefs != null) return
            val masterKey = MasterKey.Builder(context)
                .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
                .build()
            prefs = EncryptedSharedPreferences.create(
                context,
                PREFS_NAME,
                masterKey,
                EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
                EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
            )
        }
    }

    @JvmStatic
    fun load(key: String): String? = prefs?.getString(key, null)

    @JvmStatic
    fun save(key: String, value: String) {
        prefs?.edit()?.putString(key, value)?.apply()
    }
}
