package dev.miklo.remboard.pairing

import android.os.Bundle
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import dev.miklo.remboard.CoreReady
import dev.miklo.remboard.jni.RemboardNative
import dev.miklo.remboard.service.RemboardForegroundService
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

/** Scans the QR code a PC's "Add Device" screen displays and pairs with it. */
class QrScanActivity : AppCompatActivity() {

    private val scanLauncher = registerForActivityResult(ScanContract()) { result ->
        val contents = result.contents
        if (contents == null) {
            finish()
        } else {
            handleScannedQr(contents)
        }
    }

    private val cameraPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) launchScanner() else finish()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        RemboardForegroundService.ensureStarted(this)
        cameraPermissionLauncher.launch(android.Manifest.permission.CAMERA)
    }

    private fun launchScanner() {
        val options = ScanOptions().apply {
            setDesiredBarcodeFormats(ScanOptions.QR_CODE)
            setPrompt("Scan the QR code shown on your PC")
            setBeepEnabled(false)
            setOrientationLocked(true)
        }
        scanLauncher.launch(options)
    }

    private fun handleScannedQr(qrText: String) {
        lifecycleScope.launch {
            CoreReady.await()
            val resultJson = withContext(Dispatchers.IO) { RemboardNative.requestPairing(qrText) }
            val result = runCatching { JSONObject(resultJson) }.getOrNull()
            if (result?.optBoolean("ok", false) == true) {
                Toast.makeText(
                    this@QrScanActivity,
                    "Pairing requested — accept it on your PC",
                    Toast.LENGTH_LONG
                ).show()
            } else {
                val error = result?.optString("error") ?: "invalid QR code"
                Toast.makeText(this@QrScanActivity, "Pairing failed: $error", Toast.LENGTH_LONG).show()
            }
            finish()
        }
    }
}
