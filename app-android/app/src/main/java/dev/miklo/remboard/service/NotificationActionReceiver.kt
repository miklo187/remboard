package dev.miklo.remboard.service

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import dev.miklo.remboard.R
import dev.miklo.remboard.jni.RemboardNative
import dev.miklo.remboard.storage.DownloadsSaver
import java.io.File

/**
 * Handles the Copy/Save/Reject/Dismiss actions on an incoming-item
 * notification (see RemboardForegroundService.postItemNotification).
 * "Save" stages the file into the app's private cache dir via native, then
 * copies it into the shared MediaStore Downloads collection so it's
 * findable in the Files/Downloads app, and posts a confirmation
 * notification with an "Open" action.
 */
class NotificationActionReceiver : BroadcastReceiver() {

    companion object {
        const val ACTION_COPY = "dev.miklo.remboard.ACTION_COPY"
        const val ACTION_SAVE = "dev.miklo.remboard.ACTION_SAVE"
        const val ACTION_REJECT = "dev.miklo.remboard.ACTION_REJECT"
        const val ACTION_DISMISS = "dev.miklo.remboard.ACTION_DISMISS"
        const val EXTRA_ENVELOPE_ID = "envelope_id"
        const val EXTRA_TEXT = "text"
        const val EXTRA_FILE_NAME = "file_name"
        const val EXTRA_MIME_TYPE = "mime_type"
        private const val CHANNEL_ITEMS = "remboard_items"
    }

    override fun onReceive(context: Context, intent: Intent) {
        val envelopeId = intent.getStringExtra(EXTRA_ENVELOPE_ID) ?: return
        NotificationManagerCompat.from(context).cancel(envelopeId.hashCode())

        when (intent.action) {
            ACTION_COPY -> {
                val text = intent.getStringExtra(EXTRA_TEXT) ?: ""
                val clipboard = context.getSystemService(ClipboardManager::class.java)
                clipboard.setPrimaryClip(ClipData.newPlainText("remboard", text))
                RemboardNative.actOnItem(envelopeId, "copy", "")
            }
            ACTION_REJECT -> {
                RemboardNative.actOnItem(envelopeId, "reject", "")
            }
            ACTION_DISMISS -> {
                RemboardNative.actOnItem(envelopeId, "dismiss", "")
            }
            ACTION_SAVE -> {
                val fileName = intent.getStringExtra(EXTRA_FILE_NAME).takeUnless { it.isNullOrBlank() }
                    ?: "remboard-file"
                val mimeType = intent.getStringExtra(EXTRA_MIME_TYPE)
                val stagingPath = File(context.cacheDir, fileName).absolutePath
                RemboardNative.actOnItem(envelopeId, "save", stagingPath)
                val saved = DownloadsSaver.save(context, stagingPath, fileName, mimeType)
                postSaveResultNotification(context, envelopeId, fileName, saved)
            }
        }
    }

    private fun postSaveResultNotification(
        context: Context,
        envelopeId: String,
        fileName: String,
        saved: DownloadsSaver.Saved?
    ) {
        val builder = NotificationCompat.Builder(context, CHANNEL_ITEMS)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setAutoCancel(true)

        if (saved != null) {
            val openIntent = Intent(Intent.ACTION_VIEW).apply {
                setDataAndType(saved.uri, saved.mimeType)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            val openPendingIntent = PendingIntent.getActivity(
                context, envelopeId.hashCode(), openIntent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
            builder.setContentTitle("Saved to Downloads")
                .setContentText(fileName)
                .setContentIntent(openPendingIntent)
                .addAction(0, "Open", openPendingIntent)
        } else {
            builder.setContentTitle("Failed to save")
                .setContentText(fileName)
        }

        NotificationManagerCompat.from(context)
            .notify(envelopeId.hashCode() + "save".hashCode(), builder.build())
    }
}
