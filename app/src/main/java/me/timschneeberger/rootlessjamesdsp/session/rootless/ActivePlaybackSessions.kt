package me.timschneeberger.rootlessjamesdsp.session.rootless

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.AudioPlaybackConfiguration
import androidx.core.content.getSystemService
import me.timschneeberger.rootlessjamesdsp.model.AudioSessionDumpEntry
import me.timschneeberger.rootlessjamesdsp.utils.extensions.ContextExtensions.getPackageNameFromUid
import timber.log.Timber
import java.lang.reflect.Method

/**
 * Sessions that are playing right now, read from the audio service's own list
 * of players rather than parsed out of a text dump.
 *
 * The uid, session id and player state of a player are hidden API; the app
 * already lifts hidden-API restrictions at start-up, so they are read by
 * reflection. If that ever stops working this returns nothing and the dumps
 * carry on alone, exactly as before.
 */
object ActivePlaybackSessions {
    // AudioPlaybackConfiguration.PLAYER_STATE_STARTED
    private const val PLAYER_STATE_STARTED = 2

    private class Accessors(val uid: Method, val session: Method, val state: Method)

    private val accessors: Accessors? by lazy {
        try {
            val cls = AudioPlaybackConfiguration::class.java
            Accessors(
                cls.getMethod("getClientUid"),
                cls.getMethod("getSessionId"),
                cls.getMethod("getPlayerState"),
            )
        }
        catch (ex: Throwable) {
            Timber.w(ex, "Player details are not readable on this device")
            null
        }
    }

    fun query(context: Context): Map<Int, AudioSessionDumpEntry> {
        val acc = accessors ?: return emptyMap()
        val configs = try {
            context.getSystemService<AudioManager>()?.activePlaybackConfigurations
        }
        catch (ex: Exception) {
            Timber.w(ex, "Failed to read the active players")
            null
        } ?: return emptyMap()

        val sessions = hashMapOf<Int, AudioSessionDumpEntry>()
        configs.forEach { config ->
            try {
                if (acc.state.invoke(config) as Int != PLAYER_STATE_STARTED)
                    return@forEach
                val uid = acc.uid.invoke(config) as Int
                val sid = acc.session.invoke(config) as Int
                if (uid < 0 || sid <= 0)
                    return@forEach

                val pkg = context.getPackageNameFromUid(uid) ?: uid.toString()
                sessions[sid] = AudioSessionDumpEntry(
                    uid, pkg,
                    usageName(config.audioAttributes.usage),
                    "CONTENT_TYPE_UNKNOWN"
                )
            }
            catch (ex: Throwable) {
                Timber.w(ex, "Failed to read a player")
            }
        }
        return sessions
    }

    // Spelled the way the dumps spell it, which is what the filters expect
    private fun usageName(usage: Int) = when (usage) {
        AudioAttributes.USAGE_MEDIA -> "USAGE_MEDIA"
        AudioAttributes.USAGE_GAME -> "USAGE_GAME"
        AudioAttributes.USAGE_UNKNOWN -> "USAGE_UNKNOWN"
        else -> "USAGE_OTHER_$usage"
    }
}
