package tr.sonalis.core

data class VoiceDecision(
    val lifecycle: Int,
    val route: Int,
    val beginPeerProbe: Boolean,
    val cancelPeerProbe: Boolean,
    val releaseMedia: Boolean,
    val requestWakeGrant: Boolean,
)

class NativeVoiceSession(peerToPeer: Boolean, serverDenoise: Boolean) : AutoCloseable {
    private var handle = nativeCreate(peerToPeer, serverDenoise)

    init { check(handle != 0L) { "sonalis_core_allocation_failed" } }

    fun connected(monotonicMs: Long) = nativeConnected(requireHandle(), monotonicMs)
    fun disconnected() = nativeDisconnected(requireHandle())
    fun participantsChanged(count: Int, monotonicMs: Long) =
        unpack(nativeParticipantsChanged(requireHandle(), count.coerceAtLeast(0), monotonicMs))
    fun tick(monotonicMs: Long) = unpack(nativeTick(requireHandle(), monotonicMs))
    fun probeSucceeded(monotonicMs: Long) = unpack(nativeProbeSucceeded(requireHandle(), monotonicMs))
    fun probeFailed(monotonicMs: Long) = unpack(nativeProbeFailed(requireHandle(), monotonicMs))

    override fun close() {
        val value = handle
        handle = 0
        if (value != 0L) nativeDestroy(value)
    }

    private fun requireHandle() = handle.also { check(it != 0L) { "voice_session_closed" } }
    private fun unpack(value: Int) = VoiceDecision(
        lifecycle = value and 0x3,
        route = value shr 2 and 0x3,
        beginPeerProbe = value and 0x10 != 0,
        cancelPeerProbe = value and 0x20 != 0,
        releaseMedia = value and 0x40 != 0,
        requestWakeGrant = value and 0x80 != 0,
    )

    private companion object {
        init { System.loadLibrary("sonalis_android") }
        @JvmStatic private external fun nativeCreate(p2p: Boolean, serverDenoise: Boolean): Long
        @JvmStatic private external fun nativeDestroy(handle: Long)
        @JvmStatic private external fun nativeConnected(handle: Long, monotonicMs: Long)
        @JvmStatic private external fun nativeDisconnected(handle: Long)
        @JvmStatic private external fun nativeParticipantsChanged(handle: Long, count: Int, monotonicMs: Long): Int
        @JvmStatic private external fun nativeTick(handle: Long, monotonicMs: Long): Int
        @JvmStatic private external fun nativeProbeSucceeded(handle: Long, monotonicMs: Long): Int
        @JvmStatic private external fun nativeProbeFailed(handle: Long, monotonicMs: Long): Int
    }
}
