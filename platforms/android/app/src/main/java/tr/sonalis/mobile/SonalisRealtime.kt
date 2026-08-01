package tr.sonalis.mobile

import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import java.net.URI
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

class SonalisRealtime(private val api: SonalisApi,
                      private val onEvent: (String) -> Unit,
                      private val onState: (String) -> Unit) : AutoCloseable {
    private val client = OkHttpClient.Builder()
        .pingInterval(25, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.MILLISECONDS)
        .build()
    private val scheduler = Executors.newSingleThreadScheduledExecutor { runnable ->
        Thread(runnable, "SonalisRealtime").apply { isDaemon = true }
    }
    private val closed = AtomicBoolean(false)
    private val connecting = AtomicBoolean(false)
    private val attempts = AtomicInteger(0)
    private val generation = AtomicInteger(0)
    @Volatile private var socket: WebSocket? = null

    fun connect() = schedule(0)

    private fun schedule(delaySeconds: Long) {
        if (closed.get()) return
        scheduler.schedule({ open() }, delaySeconds, TimeUnit.SECONDS)
    }

    private fun open() {
        if (closed.get() || !connecting.compareAndSet(false, true)) return
        try {
            val (websocketUrl, ticket) = api.realtimeGrant()
            val base = URI(websocketUrl)
            require(base.scheme == "wss" && ticket.length in 32..128) { "invalid_realtime_grant" }
            val target = URI(base.scheme, base.userInfo, base.host, base.port, base.path,
                "ticket=${java.net.URLEncoder.encode(ticket, Charsets.UTF_8.name())}", null)
            socket?.cancel()
            val current = generation.incrementAndGet()
            socket = client.newWebSocket(Request.Builder().url(target.toString()).build(), Listener(current))
        } catch (_: Throwable) {
            connecting.set(false)
            reconnect("offline")
        }
    }

    private inner class Listener(private val current: Int) : WebSocketListener() {
        override fun onOpen(webSocket: WebSocket, response: Response) {
            if (current != generation.get()) { webSocket.cancel(); return }
            connecting.set(false)
            attempts.set(0)
            onState("connected")
        }
        override fun onMessage(webSocket: WebSocket, text: String) {
            if (current == generation.get() && text.length <= 32 * 1024) onEvent(text)
        }
        override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
            if (current != generation.get()) return
            connecting.set(false)
            if (!closed.get()) reconnect("closed")
        }
        override fun onFailure(webSocket: WebSocket, failure: Throwable, response: Response?) {
            if (current != generation.get()) return
            connecting.set(false)
            if (!closed.get()) reconnect("offline")
        }
    }

    private fun reconnect(state: String) {
        onState(state)
        val attempt = attempts.getAndIncrement().coerceAtMost(5)
        val base = (1L shl attempt).coerceAtMost(30)
        val jitter = (System.nanoTime() ushr 8) % 1_000L
        schedule(base + if (jitter >= 750) 1 else 0)
    }

    override fun close() {
        if (!closed.compareAndSet(false, true)) return
        generation.incrementAndGet()
        socket?.close(1000, "client_shutdown")
        socket = null
        scheduler.shutdownNow()
        client.dispatcher.executorService.shutdown()
    }
}
