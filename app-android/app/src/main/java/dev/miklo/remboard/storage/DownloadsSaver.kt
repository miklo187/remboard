package dev.miklo.remboard.storage

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.webkit.MimeTypeMap
import java.io.File

/**
 * Copies a file native already staged at [cacheFilePath] (via
 * Core::act_on_item(kSave, ...)) into the shared MediaStore Downloads
 * collection (API 29+), so saved files show up in the Files/Downloads app
 * at a predictable place instead of the app's private external-files
 * directory. On API < 29 (no scoped storage, MediaStore.Downloads doesn't
 * exist yet) it falls back to that private directory.
 */
object DownloadsSaver {

    data class Saved(val uri: Uri, val mimeType: String)

    fun save(context: Context, cacheFilePath: String, fileName: String, mimeTypeHint: String?): Saved? {
        val cacheFile = File(cacheFilePath)
        if (!cacheFile.exists()) return null
        val mimeType = resolveMimeType(fileName, mimeTypeHint)

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            val dir = context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS) ?: return null
            if (!dir.exists()) dir.mkdirs()
            val dest = File(dir, fileName)
            cacheFile.copyTo(dest, overwrite = true)
            cacheFile.delete()
            return Saved(Uri.fromFile(dest), mimeType)
        }

        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, fileName)
            put(MediaStore.MediaColumns.MIME_TYPE, mimeType)
            put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
            put(MediaStore.MediaColumns.IS_PENDING, 1)
        }
        val resolver = context.contentResolver
        val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values) ?: return null
        val copied = runCatching {
            resolver.openOutputStream(uri)?.use { out ->
                cacheFile.inputStream().use { it.copyTo(out) }
            } != null
        }.getOrDefault(false)
        if (!copied) {
            resolver.delete(uri, null, null)
            return null
        }
        values.clear()
        values.put(MediaStore.MediaColumns.IS_PENDING, 0)
        resolver.update(uri, values, null, null)
        cacheFile.delete()
        return Saved(uri, mimeType)
    }

    private fun resolveMimeType(fileName: String, provided: String?): String {
        if (!provided.isNullOrBlank()) return provided
        val ext = fileName.substringAfterLast('.', "").lowercase()
        return ext.takeIf { it.isNotEmpty() }
            ?.let { MimeTypeMap.getSingleton().getMimeTypeFromExtension(it) }
            ?: "application/octet-stream"
    }
}
