package dev.miklo.remboard

import dev.miklo.remboard.service.RemboardForegroundService
import kotlinx.coroutines.delay

/**
 * startForegroundService() returns before RemboardForegroundService.onCreate()
 * has actually run (and thus before RemboardNative.start() has been called),
 * so anything that wants to touch RemboardNative right after ensuring the
 * service is started should await this first.
 */
object CoreReady {
    suspend fun await() {
        while (!RemboardForegroundService.isCoreReady) {
            delay(50)
        }
    }
}
