package me.timschneeberger.rootlessjamesdsp.session.rootless

import android.content.Context
import android.content.Intent
import me.timschneeberger.rootlessjamesdsp.model.AudioSessionDumpEntry
import me.timschneeberger.rootlessjamesdsp.model.IEffectSession
import me.timschneeberger.rootlessjamesdsp.model.rootless.MutedEffectSession
import me.timschneeberger.rootlessjamesdsp.session.shared.BaseSessionDatabase
import me.timschneeberger.rootlessjamesdsp.R
import me.timschneeberger.rootlessjamesdsp.utils.MutedAudioEffectFactory
import me.timschneeberger.rootlessjamesdsp.utils.extensions.ContextExtensions.toast
import timber.log.Timber

class RootlessSessionDatabase(context: Context) : BaseSessionDatabase(context) {

    private val factory by lazy { MutedAudioEffectFactory() }
    private var sessionLossListener: OnSessionLossListener? = null
    private var appProblemListener: OnAppProblemListener? = null

    override val excludedPackages = arrayOf(
        context.packageName,

        // Non-music apps known to cause issues
        "com.google.android.googlequicksearchbox",
        "com.google.android.as",
        "com.kieronquinn.app.pixelambientmusic",
        "com.draftkings.sportsbook",
        "com.samsung.gpuwatchapp",
        "com.kalkiarts.hexabloompro"
    )

    /**
     * Apps whose audio never reaches the capture although nothing Android
     * reports says so: their manifest allows playback capture and their streams
     * carry no no-capture flags, yet the recording stays silent. Spotify is the
     * known one (also listed as unsupported upstream); checked on a Galaxy S24
     * Ultra running Android 17, where YouTube Music captured fine alongside it.
     */
    private val uncapturablePackages = setOf(
        "com.spotify.music",
    )
    private val warnedUncapturable = mutableSetOf<String>()

    /** Whether the app behind [uid] is one whose audio we can never capture. */
    fun isUncapturableUid(uid: Int): Boolean =
        context.packageManager.getPackagesForUid(uid)?.any { it in uncapturablePackages } == true

    override fun shouldAcceptSessionDump(id: Int, session: AudioSessionDumpEntry): Boolean {
        if (!session.isUsageRecordable()) {
            Timber.d("Skipped session $id due to usage ($session)")
            return false
        }
        return true
    }

    override fun shouldAddSession(id: Int, uid: Int, packageName: String): Boolean {
        if (packageName !in uncapturablePackages)
            return true
        // Muting one of these is what turned "not processed" into "silent": its
        // own output was switched off, and the capture that should have carried
        // it on received nothing. Left alone, it at least plays unprocessed.
        Timber.d("Not muting session $id of $packageName: its audio cannot be captured")
        if (warnedUncapturable.add(packageName))
            context.toast(context.getString(R.string.rootless_app_uncapturable, packageName))
        return false
    }

    override fun createSession(id: Int, uid: Int, packageName: String): MutedEffectSession? {
        val muteEffect = factory.make(id, packageName)
        if(muteEffect == null)
        {
            // Something weird happened, request user to solve issue
            appProblemListener?.onAppProblemDetected(uid)
            return null
        }
        return MutedEffectSession(uid, packageName, muteEffect)
    }

    override fun onSessionRemoved(item: IEffectSession) {
        (item as MutedEffectSession).run {
            try {
                audioMuteEffect?.enabled = false
                audioMuteEffect?.release()
            }
            catch (ex: Exception) {
                Timber.e("onSessionRemoved: effect already destroyed")
                Timber.d(ex)
            }
        }
    }

    init {
        factory.sessionLossListener = { sid, packageName ->
            sendFxCloseBroadcast(packageName, sid)
            sessionLossListener?.onSessionLost(sid)
        }
    }

    private fun sendFxCloseBroadcast(pkgName: String, sid: Int) {
        val intent = Intent("android.media.action.CLOSE_AUDIO_EFFECT_CONTROL_SESSION")
        intent.putExtra("android.media.extra.PACKAGE_NAME", pkgName)
        intent.putExtra("android.media.extra.AUDIO_SESSION", sid)
        intent.putExtra(EXTRA_IGNORE, 1)
        context.sendBroadcast(intent)

        Timber.d("Sent control session close request for $pkgName")
    }

    fun setOnSessionLossListener(newSessionLossListener: OnSessionLossListener) {
        sessionLossListener = newSessionLossListener
    }

    fun setOnAppProblemListener(newAppProblemListener: OnAppProblemListener) {
        appProblemListener = newAppProblemListener
    }

    interface OnSessionLossListener {
        fun onSessionLost(sid: Int)
    }

    interface OnAppProblemListener {
        fun onAppProblemDetected(uid: Int)
    }

    companion object {
        const val EXTRA_IGNORE = "rootlessjamesdsp.ignore"
    }
}

typealias OnRootlessSessionChangeListener = BaseSessionDatabase.OnSessionChangeListener