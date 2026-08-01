package tr.sonalis.core

object NativeCore {
    const val ABI_VERSION = 2

    init {
        System.loadLibrary("sonalis_android")
        check(nativeAbiVersion() == ABI_VERSION) { "sonalis_core_abi_mismatch" }
    }

    fun verify() = nativeAbiVersion() == ABI_VERSION

    @JvmStatic private external fun nativeAbiVersion(): Int
}
