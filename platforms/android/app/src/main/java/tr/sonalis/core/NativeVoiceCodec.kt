package tr.sonalis.core

class NativeVoiceCodec(bitrate: Int = 24_000) : AutoCloseable {
    private var handle = nativeCreate(bitrate.coerceIn(12_000, 64_000))

    init { check(handle != 0L) { "voice_codec_unavailable" } }

    fun setBitrate(value: Int) = nativeSetBitrate(requireHandle(), value.coerceIn(12_000, 64_000))
    fun encode(input: FloatArray, output: ByteArray): Int {
        require(input.size >= FRAME_SAMPLES && output.isNotEmpty())
        return nativeEncode(requireHandle(), input, output)
    }
    fun decode(input: ByteArray?, bytes: Int, output: FloatArray, fec: Boolean = false): Int {
        require(output.size >= FRAME_SAMPLES)
        return nativeDecode(requireHandle(), input, bytes.coerceAtLeast(0), output, fec)
    }

    override fun close() {
        val value = handle
        handle = 0
        if (value != 0L) nativeDestroy(value)
    }

    private fun requireHandle() = handle.also { check(it != 0L) { "voice_codec_closed" } }

    companion object {
        const val FRAME_SAMPLES = 960
        const val MAX_PACKET_BYTES = 1_275
        init { System.loadLibrary("sonalis_android") }
        @JvmStatic private external fun nativeCreate(bitrate: Int): Long
        @JvmStatic private external fun nativeDestroy(handle: Long)
        @JvmStatic private external fun nativeSetBitrate(handle: Long, bitrate: Int): Boolean
        @JvmStatic private external fun nativeEncode(handle: Long, input: FloatArray, output: ByteArray): Int
        @JvmStatic private external fun nativeDecode(handle: Long, input: ByteArray?, bytes: Int,
                                                     output: FloatArray, fec: Boolean): Int
    }
}
