package tr.sonalis.mobile

import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.URL
import java.io.InputStream
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.Base64
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.SecretKeySpec
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

data class AndroidVoiceFrame(val senderId: String, val sequence: Int, val timestamp: Long,
                             val flags: Int, val opus: ByteArray)

/** Relay transport compatible with Sonalis voice-node v3. P2P is deliberately
 * requested off until the Android ICE adapter is present; failed experiments
 * can therefore never interrupt the working encrypted relay route. */
class SonalisVoiceTransport(private val onFrame: (AndroidVoiceFrame) -> Unit,
                            private val onState: (String) -> Unit) : AutoCloseable {
    private val running = AtomicBoolean(false)
    private val outgoing = AtomicLong(0)
    private var socket: DatagramSocket? = null
    private var endpoint: InetSocketAddress? = null
    private var sessionId = ByteArray(16)
    private var key = ByteArray(32)
    private var noncePrefix = ByteArray(4)
    private var receiveThread: Thread? = null
    private var bindLatch = CountDownLatch(1)
    private var highestIncoming = 0L
    private var replayMask = 0L
    private var receivedAny = false

    fun connect(grant: MobileVoiceGrant) {
        close()
        require(!grant.p2pEnabled) { "android_relay_grant_required" }
        val joined = joinPinned(grant)
        sessionId = uuidBytes(joined.getString("sessionId"))
        key = Base64.getDecoder().decode(joined.getString("udpKey"))
        noncePrefix = Base64.getDecoder().decode(joined.getString("noncePrefix"))
        require(key.size == 32 && noncePrefix.size == 4) { "voice_session_invalid" }
        val target = InetSocketAddress(InetAddress.getByName(grant.host), grant.port)
        val datagram = DatagramSocket(null).apply {
            reuseAddress = false
            soTimeout = 500
            bind(InetSocketAddress(0))
        }
        endpoint = target
        socket = datagram
        outgoing.set(0)
        highestIncoming = 0
        replayMask = 0
        receivedAny = false
        bindLatch = CountDownLatch(1)
        running.set(true)
        receiveThread = Thread({ receiveLoop(datagram, target) }, "SonalisVoiceRx").apply {
            isDaemon = true
            priority = Thread.MAX_PRIORITY
            start()
        }
        repeat(10) {
            if (bindLatch.count == 0L) return@repeat
            sendSecure(byteArrayOf(0x01), 0x01)
            if (bindLatch.await(300, TimeUnit.MILLISECONDS)) return@repeat
        }
        if (bindLatch.count != 0L) {
            close()
            throw IllegalStateException("voice_udp_bind_timeout")
        }
        onState("connected")
    }

    fun sendAudio(sequence: Int, timestamp: Long, flags: Int, opus: ByteArray, bytes: Int): Boolean {
        if (!running.get() || bytes !in 1..1_275 || bytes > opus.size) return false
        val payload = ByteArray(8 + bytes)
        payload[0] = 0x02
        put16(payload, 1, sequence)
        put32(payload, 3, timestamp)
        payload[7] = flags.toByte()
        opus.copyInto(payload, 8, 0, bytes)
        return sendSecure(payload, 0x02)
    }

    private fun joinPinned(grant: MobileVoiceGrant): JSONObject {
        val trust = object : X509TrustManager {
            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
            override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) = Unit
            override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) {
                val leaf = chain.firstOrNull() ?: throw java.security.cert.CertificateException("certificate_missing")
                val actual = MessageDigest.getInstance("SHA-256").digest(leaf.encoded).joinToString("") { "%02x".format(it) }
                if (!actual.equals(grant.certificateFingerprint, ignoreCase = true)) {
                    throw java.security.cert.CertificateException("certificate_pin_mismatch")
                }
            }
        }
        val context = SSLContext.getInstance("TLS").apply {
            init(null, arrayOf<TrustManager>(trust), SecureRandom())
        }
        val connection = URL("https://${grant.host}:${grant.port}/join").openConnection() as HttpsURLConnection
        try {
            connection.sslSocketFactory = context.socketFactory
            connection.requestMethod = "POST"
            connection.connectTimeout = 8_000
            connection.readTimeout = 8_000
            connection.instanceFollowRedirects = false
            connection.doOutput = true
            connection.setRequestProperty("Content-Type", "application/json; charset=utf-8")
            val body = JSONObject().put("grant", grant.grant).toString().toByteArray(Charsets.UTF_8)
            connection.setFixedLengthStreamingMode(body.size)
            connection.outputStream.use { it.write(body) }
            body.fill(0)
            val status = connection.responseCode
            val bytes = readBounded(if (status in 200..299) connection.inputStream else connection.errorStream)
            require(bytes.size <= 64 * 1024 && status == 200) { "voice_join_rejected" }
            return JSONObject(bytes.toString(Charsets.UTF_8))
        } finally {
            connection.disconnect()
        }
    }

    private fun readBounded(stream: InputStream?): ByteArray {
        if (stream == null) return ByteArray(0)
        stream.use {
            val output = ByteArrayOutputStream()
            val buffer = ByteArray(8_192)
            while (true) {
                val read = it.read(buffer)
                if (read < 0) break
                require(output.size() + read <= 64 * 1024) { "voice_join_response_too_large" }
                output.write(buffer, 0, read)
            }
            return output.toByteArray()
        }
    }

    private fun sendSecure(plaintext: ByteArray, flags: Int): Boolean {
        val datagram = socket ?: return false
        val target = endpoint ?: return false
        if (!running.get() || plaintext.size > 1_400) return false
        return try {
            val sequence = outgoing.incrementAndGet()
            val header = ByteArray(28)
            header[0] = 0x53; header[1] = 0x53; header[2] = 3; header[3] = flags.toByte()
            sessionId.copyInto(header, 4)
            put64(header, 20, sequence)
            val cipher = Cipher.getInstance("ChaCha20-Poly1305")
            cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "ChaCha20"), IvParameterSpec(nonce(sequence)))
            cipher.updateAAD(header)
            val encrypted = cipher.doFinal(plaintext)
            val packet = ByteArray(header.size + encrypted.size)
            header.copyInto(packet)
            encrypted.copyInto(packet, header.size)
            datagram.send(DatagramPacket(packet, packet.size, target))
            true
        } catch (_: Exception) { false }
    }

    private fun receiveLoop(datagram: DatagramSocket, target: InetSocketAddress) {
        val storage = ByteArray(1_500)
        var lastHeartbeat = System.nanoTime()
        while (running.get()) {
            if (System.nanoTime() - lastHeartbeat >= 5_000_000_000L) {
                sendSecure(byteArrayOf(0x01), 0x01)
                lastHeartbeat = System.nanoTime()
            }
            val packet = DatagramPacket(storage, storage.size)
            try { datagram.receive(packet) }
            catch (_: java.net.SocketTimeoutException) { continue }
            catch (_: Exception) { if (running.get()) onState("offline"); break }
            if (packet.address != target.address || packet.port != target.port || packet.length < 45) continue
            val input = storage.copyOfRange(0, packet.length)
            if (input[0] != 0x53.toByte() || input[1] != 0x53.toByte() || input[2] != 3.toByte()
                || !input.copyOfRange(4, 20).contentEquals(sessionId)) continue
            val sequence = get64(input, 20)
            if (replayed(sequence)) continue
            try {
                val cipher = Cipher.getInstance("ChaCha20-Poly1305")
                cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "ChaCha20"), IvParameterSpec(nonce(sequence)))
                cipher.updateAAD(input, 0, 28)
                val plaintext = cipher.doFinal(input, 28, input.size - 28)
                acceptSequence(sequence)
                if (plaintext.isNotEmpty() && plaintext[0] == 0x11.toByte()) { bindLatch.countDown(); continue }
                if (plaintext.size >= 2 && plaintext[0] == 0x13.toByte()) { onState("sleeping"); continue }
                if (plaintext.size < 10 || plaintext[0] != 0x12.toByte()) continue
                val senderBytes = plaintext[1].toInt() and 0xff
                val metadata = 2 + senderBytes
                if (senderBytes !in 1..64 || metadata + 7 >= plaintext.size) continue
                val sender = plaintext.copyOfRange(2, metadata).toString(Charsets.UTF_8)
                onFrame(AndroidVoiceFrame(sender, get16(plaintext, metadata), get32(plaintext, metadata + 2),
                    plaintext[metadata + 6].toInt() and 0xff, plaintext.copyOfRange(metadata + 7, plaintext.size)))
            } catch (_: AEADBadTagException) {
                // Authentication failures are intentionally silent and never reach the decoder.
            } catch (_: Exception) { continue }
        }
    }

    @Synchronized private fun replayed(sequence: Long): Boolean {
        if (!receivedAny || sequence > highestIncoming) return false
        val delta = highestIncoming - sequence
        return delta >= 64 || (replayMask and (1L shl delta.toInt())) != 0L
    }

    @Synchronized private fun acceptSequence(sequence: Long) {
        if (!receivedAny) { receivedAny = true; highestIncoming = sequence; replayMask = 1; return }
        if (sequence > highestIncoming) {
            val shift = sequence - highestIncoming
            replayMask = if (shift >= 64) 1 else (replayMask shl shift.toInt()) or 1
            highestIncoming = sequence
        } else replayMask = replayMask or (1L shl (highestIncoming - sequence).toInt())
    }

    private fun nonce(sequence: Long) = ByteArray(12).also {
        noncePrefix.copyInto(it)
        put64(it, 4, sequence)
    }

    override fun close() {
        running.set(false)
        socket?.close()
        socket = null
        receiveThread?.let { if (it !== Thread.currentThread()) it.join(1_000) }
        receiveThread = null
        endpoint = null
        sessionId.fill(0); key.fill(0); noncePrefix.fill(0)
        onState("disconnected")
    }

    private fun uuidBytes(value: String): ByteArray {
        val uuid = UUID.fromString(value)
        return ByteBuffer.allocate(16).order(ByteOrder.BIG_ENDIAN)
            .putLong(uuid.mostSignificantBits).putLong(uuid.leastSignificantBits).array()
    }

    private fun put16(data: ByteArray, offset: Int, value: Int) {
        data[offset] = (value ushr 8).toByte(); data[offset + 1] = value.toByte()
    }
    private fun put32(data: ByteArray, offset: Int, value: Long) {
        for (index in 0..3) data[offset + index] = (value ushr (24 - index * 8)).toByte()
    }
    private fun put64(data: ByteArray, offset: Int, value: Long) {
        for (index in 0..7) data[offset + index] = (value ushr (56 - index * 8)).toByte()
    }
    private fun get16(data: ByteArray, offset: Int) =
        ((data[offset].toInt() and 0xff) shl 8) or (data[offset + 1].toInt() and 0xff)
    private fun get32(data: ByteArray, offset: Int): Long {
        var value = 0L
        for (index in 0..3) value = (value shl 8) or (data[offset + index].toLong() and 0xff)
        return value
    }
    private fun get64(data: ByteArray, offset: Int): Long {
        var value = 0L
        for (index in 0..7) value = (value shl 8) or (data[offset + index].toLong() and 0xff)
        return value
    }
}
