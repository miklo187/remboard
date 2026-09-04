package dev.miklo.remboard

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.VelocityTracker
import android.view.View
import android.view.ViewConfiguration
import android.widget.ArrayAdapter
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.google.android.material.snackbar.Snackbar
import dev.miklo.remboard.databinding.ActivityMainBinding
import dev.miklo.remboard.databinding.ItemDeviceBinding
import dev.miklo.remboard.databinding.ItemInboxBinding
import dev.miklo.remboard.jni.RemboardNative
import dev.miklo.remboard.pairing.QrScanActivity
import dev.miklo.remboard.service.RemboardForegroundService
import dev.miklo.remboard.storage.DownloadsSaver
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

class MainActivity : AppCompatActivity(), RemboardNative.Listener {

    private lateinit var binding: ActivityMainBinding
    private val inboxItems = mutableListOf<JSONObject>()
    private var sendDeviceUuids = listOf<String>()
    private var selectedSendDeviceUuid: String? = null

    private val notificationPermissionLauncher =
        registerForActivityResult(androidx.activity.result.contract.ActivityResultContracts.RequestPermission()) {}

    private val pickFileLauncher =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri != null) sendPickedFile(uri)
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        RemboardForegroundService.ensureStarted(this)
        requestNotificationPermissionIfNeeded()

        binding.addDeviceButton.setOnClickListener {
            startActivity(android.content.Intent(this, QrScanActivity::class.java))
        }
        binding.sendTextButton.setOnClickListener { sendComposedText() }
        binding.sendFileButton.setOnClickListener { pickFileLauncher.launch(arrayOf("*/*")) }

        lifecycleScope.launch {
            CoreReady.await()
            loadSelfInfo()
            refreshDevices()
        }
    }

    override fun onResume() {
        super.onResume()
        RemboardNative.addListener(this)
        if (RemboardForegroundService.isCoreReady) {
            lifecycleScope.launch { refreshDevices() }
        }
    }

    override fun onPause() {
        RemboardNative.removeListener(this)
        super.onPause()
    }

    private fun requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
            != PackageManager.PERMISSION_GRANTED
        ) {
            notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    private suspend fun loadSelfInfo() {
        val self = withContext(Dispatchers.IO) { JSONObject(RemboardNative.selfInfo()) }
        binding.selfInfoText.text = self.optString("display_name")
    }

    private suspend fun refreshDevices() {
        val devices = withContext(Dispatchers.IO) { JSONArray(RemboardNative.listDevices()) }
        renderDevices(devices)
    }

    private fun renderDevices(devices: JSONArray) {
        binding.deviceListContainer.removeAllViews()
        for (i in 0 until devices.length()) {
            val device = devices.getJSONObject(i)
            val row = ItemDeviceBinding.inflate(LayoutInflater.from(this), binding.deviceListContainer, false)
            row.deviceName.text = device.optString("display_name")
            row.onlineDot.setBackgroundResource(
                if (device.optBoolean("online")) R.drawable.dot_online else R.drawable.dot_offline
            )
            row.removeButton.setOnClickListener {
                lifecycleScope.launch {
                    withContext(Dispatchers.IO) {
                        RemboardNative.removeDevice(device.optString("device_uuid"))
                    }
                    refreshDevices()
                }
            }
            binding.deviceListContainer.addView(row.root)
        }
        updateSendDeviceDropdown(devices)
    }

    private fun updateSendDeviceDropdown(devices: JSONArray) {
        val uuids = mutableListOf<String>()
        val names = mutableListOf<String>()
        for (i in 0 until devices.length()) {
            val device = devices.getJSONObject(i)
            uuids.add(device.optString("device_uuid"))
            names.add(device.optString("display_name"))
        }
        sendDeviceUuids = uuids

        val hasDevices = uuids.isNotEmpty()
        binding.sendSection.visibility = if (hasDevices) View.VISIBLE else View.GONE
        binding.sendNoDevicesText.visibility = if (hasDevices) View.GONE else View.VISIBLE
        if (!hasDevices) {
            selectedSendDeviceUuid = null
            return
        }

        binding.sendDeviceDropdown.setAdapter(
            ArrayAdapter(this, android.R.layout.simple_list_item_1, names)
        )
        val currentIndex = uuids.indexOf(selectedSendDeviceUuid).let { if (it >= 0) it else 0 }
        selectedSendDeviceUuid = uuids[currentIndex]
        binding.sendDeviceDropdown.setText(names[currentIndex], false)
        binding.sendDeviceDropdown.setOnItemClickListener { _, _, position, _ ->
            selectedSendDeviceUuid = uuids[position]
        }
    }

    private fun sendComposedText() {
        val deviceUuid = selectedSendDeviceUuid
        if (deviceUuid == null) {
            Toast.makeText(this, "Pick a device first", Toast.LENGTH_SHORT).show()
            return
        }
        val text = binding.sendTextInput.text?.toString()?.trim().orEmpty()
        if (text.isEmpty()) return
        lifecycleScope.launch {
            val ok = withContext(Dispatchers.IO) {
                JSONObject(RemboardNative.sendText(deviceUuid, text, "")).optBoolean("ok", false)
            }
            if (ok) {
                binding.sendTextInput.setText("")
                Snackbar.make(binding.root, "Sent", Snackbar.LENGTH_SHORT).show()
            } else {
                Snackbar.make(binding.root, "Failed to send", Snackbar.LENGTH_LONG).show()
            }
        }
    }

    private fun sendPickedFile(uri: Uri) {
        val deviceUuid = selectedSendDeviceUuid
        if (deviceUuid == null) {
            Toast.makeText(this, "Pick a device first", Toast.LENGTH_SHORT).show()
            return
        }
        lifecycleScope.launch {
            val fileName = withContext(Dispatchers.IO) { queryDisplayName(uri) } ?: "remboard-file"
            val ok = withContext(Dispatchers.IO) {
                val dest = File(cacheDir, fileName)
                val copied = try {
                    contentResolver.openInputStream(uri)?.use { input ->
                        dest.outputStream().use { output -> input.copyTo(output) }
                    }
                    true
                } catch (e: Exception) {
                    false
                }
                if (!copied) return@withContext false
                JSONObject(RemboardNative.sendFile(deviceUuid, dest.absolutePath)).optBoolean("ok", false)
            }
            Snackbar.make(
                binding.root,
                if (ok) "Sent $fileName" else "Failed to send $fileName",
                Snackbar.LENGTH_LONG
            ).show()
        }
    }

    private fun queryDisplayName(uri: Uri): String? {
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (idx >= 0 && cursor.moveToFirst()) return cursor.getString(idx)
        }
        return uri.lastPathSegment
    }

    override fun onIncomingItem(itemJson: String) {
        runOnUiThread {
            inboxItems.add(0, JSONObject(itemJson))
            renderInbox()
        }
    }

    override fun onTransferProgress(progressJson: String) {}

    override fun onPeerStatusChanged(deviceJson: String) {
        runOnUiThread { lifecycleScope.launch { refreshDevices() } }
    }

    private fun renderInbox() {
        binding.inboxContainer.removeAllViews()
        for (item in inboxItems) {
            val row = ItemInboxBinding.inflate(LayoutInflater.from(this), binding.inboxContainer, false)
            val kind = item.optString("kind")
            val from = item.optString("from_display_name", "Unknown device")
            row.itemMeta.text = from
            row.itemBody.text = if (kind == "file") {
                "📎 ${item.optString("file_name")}"
            } else {
                item.optString("text")
            }

            row.recycleBinButton.setOnClickListener { actOnItem(item, "reject", row.root) }
            attachSwipeToRemove(row.root) { actOnItem(item, "reject", row.root) }
            if (kind == "file") {
                row.primaryActionButton.text = getString(R.string.action_save)
                row.primaryActionButton.setOnClickListener { saveFileItem(item, row.root) }
            } else {
                row.primaryActionButton.text = getString(R.string.action_copy)
                row.primaryActionButton.setOnClickListener { copyTextItem(item, row.root) }
            }
            binding.inboxContainer.addView(row.root)
        }
    }

    private fun copyTextItem(item: JSONObject, view: View) {
        val clipboard = getSystemService(android.content.ClipboardManager::class.java)
        clipboard.setPrimaryClip(android.content.ClipData.newPlainText("remboard", item.optString("text")))
        actOnItem(item, "copy", view)
        Toast.makeText(this, "Copied", Toast.LENGTH_SHORT).show()
    }

    private fun saveFileItem(item: JSONObject, view: View) {
        val fileName = item.optString("file_name").ifBlank { "remboard-file" }
        val envelopeId = item.optString("envelope_id")
        val stagingPath = File(cacheDir, fileName).absolutePath
        lifecycleScope.launch {
            val saved = withContext(Dispatchers.IO) {
                RemboardNative.actOnItem(envelopeId, "save", stagingPath)
                DownloadsSaver.save(this@MainActivity, stagingPath, fileName, item.optString("mime_type"))
            }
            if (saved == null) {
                Snackbar.make(binding.root, "Failed to save $fileName", Snackbar.LENGTH_LONG).show()
                return@launch
            }
            Snackbar.make(binding.root, "Saved to Downloads/$fileName", Snackbar.LENGTH_LONG)
                .setAction("Open") {
                    val openIntent = Intent(Intent.ACTION_VIEW).apply {
                        setDataAndType(saved.uri, saved.mimeType)
                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    }
                    runCatching { startActivity(openIntent) }
                }
                .show()
            removeInboxItem(item, view)
        }
    }

    private fun actOnItem(item: JSONObject, action: String, view: View) {
        lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                RemboardNative.actOnItem(item.optString("envelope_id"), action, "")
            }
            if (action == "reject" || action == "dismiss") removeInboxItem(item, view)
        }
    }

    /**
     * Lets the user swipe an inbox row left or right to remove it, mirroring
     * the recycle-bin button. Only engages when the drag starts on the card
     * itself (not on a child button), so button clicks and vertical list
     * scrolling are unaffected.
     */
    private fun attachSwipeToRemove(view: View, onSwipedAway: () -> Unit) {
        val touchSlop = ViewConfiguration.get(view.context).scaledTouchSlop
        val minFlingVelocity = ViewConfiguration.get(view.context).scaledMinimumFlingVelocity
        var downX = 0f
        var downY = 0f
        var startTranslation = 0f
        var dragging = false
        var velocityTracker: VelocityTracker? = null

        view.setOnTouchListener { v, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downX = event.rawX
                    downY = event.rawY
                    startTranslation = v.translationX
                    dragging = false
                    velocityTracker = VelocityTracker.obtain().apply { addMovement(event) }
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    velocityTracker?.addMovement(event)
                    val dx = event.rawX - downX
                    val dy = event.rawY - downY
                    if (!dragging && kotlin.math.abs(dx) > touchSlop && kotlin.math.abs(dx) > kotlin.math.abs(dy)) {
                        dragging = true
                        v.parent?.requestDisallowInterceptTouchEvent(true)
                    }
                    if (dragging) {
                        v.translationX = startTranslation + dx
                        v.alpha = 1f - (kotlin.math.abs(v.translationX) / v.width).coerceIn(0f, 1f) * 0.7f
                    }
                    dragging
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    val wasDragging = dragging
                    if (wasDragging) {
                        velocityTracker?.addMovement(event)
                        velocityTracker?.computeCurrentVelocity(1000)
                        val flungAway = kotlin.math.abs(velocityTracker?.xVelocity ?: 0f) > minFlingVelocity
                        val pastThreshold = kotlin.math.abs(v.translationX) > v.width * 0.35f
                        if (event.actionMasked == MotionEvent.ACTION_UP && (pastThreshold || flungAway)) {
                            val target = if (v.translationX > 0) v.width.toFloat() else -v.width.toFloat()
                            v.animate().translationX(target).alpha(0f).setDuration(200)
                                .withEndAction { onSwipedAway() }.start()
                        } else {
                            v.animate().translationX(0f).alpha(1f).setDuration(150).start()
                        }
                    }
                    velocityTracker?.recycle()
                    velocityTracker = null
                    dragging = false
                    wasDragging
                }
                else -> false
            }
        }
    }

    private fun removeInboxItem(item: JSONObject, view: View) {
        inboxItems.removeAll { it.optString("envelope_id") == item.optString("envelope_id") }
        binding.inboxContainer.removeView(view)
    }
}
