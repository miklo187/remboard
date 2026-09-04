package dev.miklo.remboard.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import dev.miklo.remboard.MainActivity
import dev.miklo.remboard.R
import dev.miklo.remboard.discovery.NsdDiscovery
import dev.miklo.remboard.jni.RemboardNative
import dev.miklo.remboard.storage.EncryptedDeviceStore
import org.json.JSONArray
import org.json.JSONObject

/**
 * Always-on foreground service: owns the native Core's lifecycle (see
 * jni/RemboardNative.kt) and its persistent ROUTER socket, so PC -> phone
 * sends arrive even when no Activity is on screen. Posts a notification
 * per incoming item with Copy/Save/Reject actions, handled by
 * NotificationActionReceiver.
 */
class RemboardForegroundService : Service(), RemboardNative.Listener {

    companion object {
        private const val CHANNEL_CONNECTION = "remboard_connection"
        private const val CHANNEL_ITEMS = "remboard_items"
        private const val NOTIF_ID_CONNECTION = 1
        private const val LISTEN_PORT = 49321

        private var coreStarted = false

        // Polled by activities before touching RemboardNative directly, since
        // startForegroundService() returns before onCreate() has actually run.
        @Volatile
        var isCoreReady = false
            private set

        fun ensureStarted(context: Context) {
            val intent = Intent(context, RemboardForegroundService::class.java)
            ContextCompat.startForegroundService(context, intent)
        }
    }

    override fun onCreate() {
        super.onCreate()
        EncryptedDeviceStore.init(applicationContext)
        NsdDiscovery.init(applicationContext)
        createNotificationChannels()
        startForeground(NOTIF_ID_CONNECTION, buildConnectionNotification())

        if (!coreStarted) {
            coreStarted = true
            RemboardNative.start(filesDir.absolutePath, deviceDisplayName(), LISTEN_PORT)
        }
        isCoreReady = true
        RemboardNative.addListener(this)
        refreshConnectionNotification()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY

    override fun onDestroy() {
        RemboardNative.removeListener(this)
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun deviceDisplayName(): String {
        val model = Build.MODEL ?: "Android"
        return if (model.startsWith(Build.MANUFACTURER, ignoreCase = true)) model
        else "${Build.MANUFACTURER} $model"
    }

    private fun createNotificationChannels() {
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(
                CHANNEL_CONNECTION,
                getString(R.string.notification_channel_connection),
                NotificationManager.IMPORTANCE_LOW
            )
        )
        manager.createNotificationChannel(
            NotificationChannel(
                CHANNEL_ITEMS,
                getString(R.string.notification_channel_items),
                NotificationManager.IMPORTANCE_HIGH
            )
        )
    }

    private fun buildConnectionNotification(): Notification {
        val openIntent = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_CONNECTION)
            .setContentTitle(getString(R.string.notification_connected_title))
            .setContentText(connectionStatusText())
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentIntent(openIntent)
            .setOngoing(true)
            .build()
    }

    private fun connectionStatusText(): String {
        // Not ready yet (this runs before RemboardNative.start() on first
        // launch): show the neutral default rather than an empty list.
        if (!isCoreReady) return getString(R.string.notification_connected_text_none)
        val devices = runCatching { JSONArray(RemboardNative.listDevices()) }.getOrNull()
        return when (devices?.length() ?: 0) {
            0 -> getString(R.string.notification_connected_text_none)
            1 -> getString(
                R.string.notification_connected_text_one,
                devices!!.getJSONObject(0).optString("display_name")
            )
            else -> getString(R.string.notification_connected_text_many, devices!!.length())
        }
    }

    override fun onIncomingItem(itemJson: String) {
        postItemNotification(JSONObject(itemJson))
    }

    override fun onTransferProgress(progressJson: String) {}

    override fun onPeerStatusChanged(deviceJson: String) {
        refreshConnectionNotification()
    }

    /** Rebuilds the persistent connection notification to reflect the
     * current paired-devices list, so tapping it isn't a mystery -- it
     * shows who you're paired with (see MainActivity for the actual list). */
    private fun refreshConnectionNotification() {
        NotificationManagerCompat.from(this).notify(NOTIF_ID_CONNECTION, buildConnectionNotification())
    }

    private fun postItemNotification(item: JSONObject) {
        val envelopeId = item.getString("envelope_id")
        val kind = item.getString("kind")
        val from = item.optString("from_display_name", "Unknown device")
        val text = if (kind == "file") "File: ${item.optString("file_name")}" else item.optString("text")

        val openIntent = PendingIntent.getActivity(
            this, envelopeId.hashCode(), Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val builder = NotificationCompat.Builder(this, CHANNEL_ITEMS)
            .setContentTitle("From $from")
            .setContentText(text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(text))
            .setSmallIcon(R.mipmap.ic_launcher)
            .setAutoCancel(true)
            .setContentIntent(openIntent)
            .addAction(0, getString(R.string.action_reject), actionIntent(NotificationActionReceiver.ACTION_REJECT, item))

        if (kind == "file") {
            builder.addAction(0, getString(R.string.action_save), actionIntent(NotificationActionReceiver.ACTION_SAVE, item))
        } else {
            builder.addAction(0, getString(R.string.action_copy), actionIntent(NotificationActionReceiver.ACTION_COPY, item))
        }

        NotificationManagerCompat.from(this).notify(envelopeId.hashCode(), builder.build())
    }

    private fun actionIntent(action: String, item: JSONObject): PendingIntent {
        val envelopeId = item.getString("envelope_id")
        val intent = Intent(this, NotificationActionReceiver::class.java).apply {
            this.action = action
            putExtra(NotificationActionReceiver.EXTRA_ENVELOPE_ID, envelopeId)
            putExtra(NotificationActionReceiver.EXTRA_TEXT, item.optString("text"))
            putExtra(NotificationActionReceiver.EXTRA_FILE_NAME, item.optString("file_name"))
            putExtra(NotificationActionReceiver.EXTRA_MIME_TYPE, item.optString("mime_type"))
        }
        return PendingIntent.getBroadcast(
            this, envelopeId.hashCode() + action.hashCode(), intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
    }
}
