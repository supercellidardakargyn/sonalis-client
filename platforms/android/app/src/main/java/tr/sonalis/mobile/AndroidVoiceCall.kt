package tr.sonalis.mobile

import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat
import tr.sonalis.core.NativeAudioEngine
import tr.sonalis.core.NativeVoiceCodec
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

class AndroidVoiceCall(private val context: Context, private val api: SonalisApi,
                       private val state: (String) -> Unit) : AutoCloseable {
    private val running = AtomicBoolean(false)
    private var audio: NativeAudioEngine? = null
    private var encoder: NativeVoiceCodec? = null
    private var transport: SonalisVoiceTransport? = null
    private var mediaThread: Thread? = null
    private val decoders = ConcurrentHashMap<String, NativeVoiceCodec>()

    fun connect(roomId: String, channelId: String, captureEnabled: Boolean) {
        close()
        state("connecting")
        val grant = api.voiceGrant(roomId, channelId, serverDenoise = false, peerToPeer = false)
        val output = NativeAudioEngine()
        check(output.start(captureEnabled)) { "audio_start_failed" }
        val voiceEncoder = if (captureEnabled && grant.canSpeak) NativeVoiceCodec(grant.bitrate) else null
        val voiceTransport = SonalisVoiceTransport({ frame ->
            val decoder = decoders.computeIfAbsent(frame.senderId) { NativeVoiceCodec(grant.bitrate) }
            val pcm = FloatArray(NativeVoiceCodec.FRAME_SAMPLES)
            val decoded = decoder.decode(frame.opus, frame.opus.size, pcm)
            if (decoded > 0) output.writePlayback(pcm, decoded)
        }, state)
        try {
            voiceTransport.connect(grant)
        } catch (error: Throwable) {
            voiceTransport.close(); voiceEncoder?.close(); output.close()
            throw error
        }
        audio = output
        encoder = voiceEncoder
        transport = voiceTransport
        running.set(true)
        if (captureEnabled && voiceEncoder != null) {
            ContextCompat.startForegroundService(context, Intent(context, VoiceForegroundService::class.java))
            mediaThread = Thread({ captureLoop(output, voiceEncoder, voiceTransport) }, "SonalisVoiceTx").apply {
                isDaemon = true
                priority = Thread.MAX_PRIORITY
                start()
            }
        }
        state(if (captureEnabled) "connected" else "listen_only")
    }

    private fun captureLoop(audio: NativeAudioEngine, codec: NativeVoiceCodec,
                            transport: SonalisVoiceTransport) {
        val frame = FloatArray(NativeVoiceCodec.FRAME_SAMPLES)
        val scratch = FloatArray(NativeVoiceCodec.FRAME_SAMPLES)
        val packet = ByteArray(NativeVoiceCodec.MAX_PACKET_BYTES)
        var filled = 0
        var sequence = 0
        var timestamp = 0L
        var hangover = 0
        var wasActive = false
        while (running.get() && audio.healthy()) {
            val read = audio.readCapture(scratch)
            if (read <= 0) { Thread.sleep(2); continue }
            var source = 0
            while (source < read) {
                val copied = minOf(frame.size - filled, read - source)
                scratch.copyInto(frame, filled, source, source + copied)
                source += copied
                filled += copied
                if (filled < frame.size) continue
                var sum = 0.0
                for (sample in frame) {
                    val value = sample.coerceIn(-1f, 1f)
                    sum += value * value
                }
                val talking = kotlin.math.sqrt(sum / frame.size) >= 0.006
                if (talking) hangover = 15 else if (hangover > 0) hangover--
                val active = talking || hangover > 0
                if (active) {
                    val bytes = codec.encode(frame, packet)
                    if (bytes > 0) transport.sendAudio(sequence++, timestamp, if (!wasActive) 1 else 0, packet, bytes)
                }
                wasActive = active
                timestamp = (timestamp + NativeVoiceCodec.FRAME_SAMPLES) and 0xffff_ffffL
                filled = 0
            }
        }
        if (running.get()) state("audio_failed")
    }

    override fun close() {
        running.set(false)
        mediaThread?.let { if (it !== Thread.currentThread()) it.join(1_000) }
        mediaThread = null
        transport?.close(); transport = null
        encoder?.close(); encoder = null
        decoders.values.forEach { it.close() }
        decoders.clear()
        audio?.close(); audio = null
        context.stopService(Intent(context, VoiceForegroundService::class.java))
    }
}
