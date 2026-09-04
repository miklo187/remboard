package dev.miklo.remboard

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.LayoutInflater
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import dev.miklo.remboard.databinding.ActivityShareReceiverBinding
import dev.miklo.remboard.databinding.ItemDeviceShareBinding
import dev.miklo.remboard.jni.RemboardNative
import dev.miklo.remboard.service.RemboardForegroundService
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** The "Send to remboard" picker shown by Android's share sheet. */
class ShareReceiverActivity : AppCompatActivity() {

    private lateinit var binding: ActivityShareReceiverBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityShareReceiverBinding.inflate(layoutInflater)
        setContentView(binding.root)

        RemboardForegroundService.ensureStarted(this)
        binding.sharePreviewText.text = describeSharedContent()

        lifecycleScope.launch {
            CoreReady.await()
            val devices = withContext(Dispatchers.IO) { JSONArray(RemboardNative.listDevices()) }
            if (devices.length() == 0) {
                binding.noDevicesText.visibility = android.view.View.VISIBLE
                binding.tapHintText.visibility = android.view.View.GONE
                return@launch
            }
            for (i in 0 until devices.length()) {
                val device = devices.getJSONObject(i)
                val row = ItemDeviceShareBinding.inflate(LayoutInflater.from(this@ShareReceiverActivity), binding.deviceListContainer, false)
                row.deviceName.text = device.optString("display_name")
                row.onlineDot.setBackgroundResource(
                    if (device.optBoolean("online")) R.drawable.dot_online else R.drawable.dot_offline
                )
                row.root.setOnClickListener { sendSharedContentTo(device.optString("device_uuid")) }
                binding.deviceListContainer.addView(row.root)
            }
        }
    }

    /** A short human-readable summary of what's about to be sent, shown above the device list. */
    private fun describeSharedContent(): String {
        if (intent.action == Intent.ACTION_SEND && intent.type == "text/plain") {
            val text = intent.getStringExtra(Intent.EXTRA_TEXT).orEmpty()
            return "“$text”"
        }
        val uris = sharedUris()
        return when (uris.size) {
            0 -> ""
            1 -> "📎 ${queryDisplayName(uris[0]) ?: "1 file"}"
            else -> "📎 ${uris.size} files"
        }
    }

    private fun sharedUris(): List<Uri> = when (intent.action) {
        Intent.ACTION_SEND -> intent.getParcelableExtraCompat<Uri>(Intent.EXTRA_STREAM)?.let { listOf(it) } ?: emptyList()
        Intent.ACTION_SEND_MULTIPLE -> intent.getParcelableArrayListExtraCompat<Uri>(Intent.EXTRA_STREAM) ?: emptyList()
        else -> emptyList()
    }

    private fun sendSharedContentTo(deviceUuid: String) {
        lifecycleScope.launch {
            val text = if (intent.action == Intent.ACTION_SEND && intent.type == "text/plain") {
                intent.getStringExtra(Intent.EXTRA_TEXT)
            } else null

            val ok = withContext(Dispatchers.IO) {
                if (text != null) {
                    sendText(deviceUuid, text)
                } else {
                    sendSharedFiles(deviceUuid)
                }
            }

            Toast.makeText(
                this@ShareReceiverActivity,
                if (ok) "Sent" else "Failed to send",
                Toast.LENGTH_SHORT
            ).show()
            finish()
        }
    }

    private fun sendText(deviceUuid: String, text: String): Boolean {
        val callerPackage = referrer?.host ?: ""
        val result = JSONObject(RemboardNative.sendText(deviceUuid, text, callerPackage))
        return result.optBoolean("ok", false)
    }

    private fun sendSharedFiles(deviceUuid: String): Boolean {
        val uris = sharedUris()
        if (uris.isEmpty()) return false

        var allOk = true
        for (uri in uris) {
            val file = copyUriToCache(uri)
            if (file == null) {
                allOk = false
                continue
            }
            val result = JSONObject(RemboardNative.sendFile(deviceUuid, file.absolutePath))
            if (!result.optBoolean("ok", false)) allOk = false
        }
        return allOk
    }

    private fun copyUriToCache(uri: Uri): File? {
        val name = queryDisplayName(uri) ?: "shared-file"
        val dest = File(cacheDir, name)
        return try {
            contentResolver.openInputStream(uri)?.use { input ->
                dest.outputStream().use { output -> input.copyTo(output) }
            }
            dest
        } catch (e: Exception) {
            null
        }
    }

    private fun queryDisplayName(uri: Uri): String? {
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (idx >= 0 && cursor.moveToFirst()) return cursor.getString(idx)
        }
        return uri.lastPathSegment
    }

    @Suppress("DEPRECATION")
    private fun <T : android.os.Parcelable> Intent.getParcelableExtraCompat(name: String): T? {
        return if (android.os.Build.VERSION.SDK_INT >= 33) {
            getParcelableExtra(name, android.os.Parcelable::class.java) as? T
        } else {
            getParcelableExtra(name)
        }
    }

    @Suppress("DEPRECATION")
    private fun <T : android.os.Parcelable> Intent.getParcelableArrayListExtraCompat(name: String): ArrayList<T>? {
        return if (android.os.Build.VERSION.SDK_INT >= 33) {
            getParcelableArrayListExtra(name, android.os.Parcelable::class.java) as? ArrayList<T>
        } else {
            getParcelableArrayListExtra(name)
        }
    }
}
