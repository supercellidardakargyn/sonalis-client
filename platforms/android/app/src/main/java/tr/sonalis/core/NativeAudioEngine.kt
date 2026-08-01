package tr.sonalis.core

class NativeAudioEngine : AutoCloseable {
    private var handle = nativeCreate()

    init { check(handle != 0L) { "audio_allocation_failed" } }

    fun start(captureEnabled: Boolean): Boolean = nativeStart(requireHandle(), captureEnabled)
    fun stop() = nativeStop(requireHandle())
    fun readCapture(output: FloatArray): Int = nativeReadCapture(requireHandle(), output)
    fun writePlayback(input: FloatArray, count: Int = input.size): Int =
        nativeWritePlayback(requireHandle(), input, count.coerceIn(0, input.size))
    fun healthy(): Boolean = nativeHealthy(requireHandle())

    override fun close() {
        val value = handle
        handle = 0
        if (value != 0L) nativeDestroy(value)
    }

    private fun requireHandle() = handle.also { check(it != 0L) { "audio_engine_closed" } }

    private companion object {
        init { System.loadLibrary("sonalis_android") }
        @JvmStatic private external fun nativeCreate(): Long
        @JvmStatic private external fun nativeDestroy(handle: Long)
        @JvmStatic private external fun nativeStart(handle: Long, captureEnabled: Boolean): Boolean
        @JvmStatic private external fun nativeStop(handle: Long)
        @JvmStatic private external fun nativeReadCapture(handle: Long, output: FloatArray): Int
        @JvmStatic private external fun nativeWritePlayback(handle: Long, input: FloatArray, count: Int): Int
        @JvmStatic private external fun nativeHealthy(handle: Long): Boolean
    }
}
