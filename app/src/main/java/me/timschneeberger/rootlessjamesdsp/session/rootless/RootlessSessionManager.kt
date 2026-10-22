package me.timschneeberger.rootlessjamesdsp.session.rootless

import android.content.*
import me.timschneeberger.rootlessjamesdsp.session.dump.DumpManager
import me.timschneeberger.rootlessjamesdsp.session.dump.data.AudioServiceDump
import me.timschneeberger.rootlessjamesdsp.session.dump.data.ISessionInfoDump
import me.timschneeberger.rootlessjamesdsp.session.dump.data.ISessionPolicyInfoDump
import me.timschneeberger.rootlessjamesdsp.session.shared.BaseSessionManager
import timber.log.Timber


class RootlessSessionManager(context: Context) : BaseSessionManager(context)
{
    // Session database
    val sessionDatabase: RootlessSessionDatabase = RootlessSessionDatabase(context)
    // Session policy database
    val sessionPolicyDatabase: SessionRecordingPolicyManager = SessionRecordingPolicyManager(context)

    override fun destroy()
    {
        super.destroy()
        sessionDatabase.destroy()
        sessionPolicyDatabase.destroy()
    }

    override fun handleSessionDump(sessionDump: ISessionInfoDump?) {
        if(sessionDump is ISessionPolicyInfoDump) {
            sessionPolicyDatabase.update(sessionDump)
        }
        else {
            dumpManager.dumpCaptureAllowlistLog()?.let { sessionPolicyDatabase.update(it) }
        }

        // The service dumps are parsed text, and their format moves between
        // Android versions and vendors. Anything playing right now is also
        // listed by the audio service itself, so whatever a dump missed is
        // added from there; otherwise an app the parser cannot see is never
        // taken over and plays unprocessed while we report "idle".
        val playing = ActivePlaybackSessions.query(context)
        val missed = playing.filterKeys { sessionDump?.sessions?.containsKey(it) != true }
        if (missed.isNotEmpty())
            Timber.d("Sessions playing but missing from the dump: ${missed.entries.joinToString { "${it.key}=${it.value.packageName}" }}")

        val merged = when {
            sessionDump == null && missed.isEmpty() -> return
            sessionDump == null -> AudioServiceDump(HashMap(missed))
            missed.isEmpty() -> sessionDump
            else -> { sessionDump.sessions.putAll(missed); sessionDump }
        }
        sessionDatabase.update(merged)
    }

    override fun onDumpMethodChange(method: DumpManager.Method) {
        sessionDatabase.clearSessions()
        sessionPolicyDatabase.clearSessions()
        super.onDumpMethodChange(method)
    }
}