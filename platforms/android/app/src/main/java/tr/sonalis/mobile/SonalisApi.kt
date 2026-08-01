package tr.sonalis.mobile

import android.os.SystemClock
import org.json.JSONArray
import org.json.JSONObject
import tr.sonalis.core.SonalisSecureStore
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URI
import java.net.URL
import java.nio.charset.StandardCharsets

data class MobileUser(val id: String, val username: String, val nickname: String)
data class MobileRoom(val id: String, val name: String, val role: String)
data class MobileChannel(val id: String, val roomId: String, val name: String, val type: String,
                         val unreadCount: Int, val mentionCount: Int)
data class MobileMessage(val id: String, val channelId: String, val senderId: String,
                         val ciphertext: String, val nonce: String, val signature: String,
                         val createdAt: String)
data class MobileVoiceGrant(val grant: String, val roomId: String, val channelId: String,
                            val host: String, val port: Int, val certificateFingerprint: String,
                            val bitrate: Int, val serverDenoise: Boolean, val p2pEnabled: Boolean,
                            val canSpeak: Boolean, val routeType: String)
data class MobileSession(val user: MobileUser, val accessToken: String, val refreshToken: String,
                         val expiresAtElapsedMs: Long)

class SonalisApi(private val secureStore: SonalisSecureStore,
                 origin: String = "https://sonalis.tr") {
    private val base = URI(origin).also {
        require(it.scheme == "https" && !it.host.isNullOrBlank() && it.userInfo == null) { "invalid_origin" }
    }.toString().removeSuffix("/")
    private val refreshLock = Any()
    @Volatile private var accessToken = ""
    @Volatile private var expiresAtElapsedMs = 0L

    fun login(login: String, password: String): MobileSession {
        val body = JSONObject()
            .put("login", login.trim())
            .put("password", password)
            .put("deviceName", "Android")
            .put("clientVersion", "5.2.0")
        val json = request("POST", "/api/v1/auth/login", body, authenticated = false)
        val userJson = json.getJSONObject("user")
        val refresh = json.getString("refreshToken")
        val expires = SystemClock.elapsedRealtime() + json.optLong("expiresIn", 900L) * 1_000L
        accessToken = json.getString("accessToken")
        expiresAtElapsedMs = expires
        secureStore.put(REFRESH_TOKEN, refresh.toByteArray(StandardCharsets.UTF_8))
        return MobileSession(
            MobileUser(userJson.getString("id"), userJson.getString("username"),
                userJson.optString("nickname", userJson.getString("username"))),
            accessToken, refresh, expires)
    }

    fun restore(): Boolean {
        val refresh = secureStore.get(REFRESH_TOKEN)?.toString(StandardCharsets.UTF_8) ?: return false
        return try {
            refresh(refresh)
            true
        } catch (_: ApiException) {
            false
        }
    }

    fun logout() {
        val refresh = secureStore.get(REFRESH_TOKEN)?.toString(StandardCharsets.UTF_8)
        try {
            if (!refresh.isNullOrEmpty()) request("POST", "/api/v1/auth/logout",
                JSONObject().put("refreshToken", refresh), authenticated = false)
        } finally {
            accessToken = ""
            expiresAtElapsedMs = 0
            secureStore.erase(REFRESH_TOKEN)
        }
    }

    fun rooms(): List<MobileRoom> {
        val rows = request("GET", "/api/v1/me/rooms").getJSONArray("rooms")
        return (0 until rows.length()).map { index ->
            rows.getJSONObject(index).run {
                MobileRoom(getString("id"), getString("name"), optString("role", "member"))
            }
        }.take(100)
    }

    fun channels(roomId: String): List<MobileChannel> {
        requireUuid(roomId)
        val rows = request("GET", "/api/v1/rooms/$roomId/overview").getJSONArray("channels")
        return (0 until rows.length()).map { index ->
            rows.getJSONObject(index).run {
                MobileChannel(getString("id"), roomId, getString("name"), getString("type"),
                    optInt("unreadCount"), optInt("mentionCount"))
            }
        }.take(100)
    }

    fun messages(channelId: String, beforeCursor: String? = null): List<MobileMessage> {
        requireUuid(channelId)
        val suffix = beforeCursor?.takeIf { it.length <= 256 }?.let {
            "?beforeCursor=${java.net.URLEncoder.encode(it, StandardCharsets.UTF_8.name())}&limit=50"
        } ?: "?limit=50"
        val rows = request("GET", "/api/v1/channels/$channelId/messages$suffix").getJSONArray("messages")
        return (0 until rows.length()).map { index ->
            rows.getJSONObject(index).run {
                MobileMessage(getString("id"), optString("channelId", channelId), getString("senderId"),
                    getString("ciphertext"), getString("nonce"), getString("signature"), getString("createdAt"))
            }
        }.take(50)
    }

    fun realtimeGrant(): Pair<String, String> {
        val json = request("POST", "/api/v1/realtime/grant", JSONObject())
        val url = URI(json.getString("websocketUrl"))
        require(url.scheme == "wss" && url.host == URI(base).host) { "invalid_realtime_origin" }
        return url.toString() to json.getString("ticket")
    }

    fun voiceGrant(roomId: String, channelId: String, serverDenoise: Boolean,
                   peerToPeer: Boolean): MobileVoiceGrant {
        requireUuid(roomId)
        requireUuid(channelId)
        val json = request("POST", "/api/v1/voice/join-grant", JSONObject()
            .put("roomId", roomId)
            .put("channelId", channelId)
            .put("serverDenoiseRequested", serverDenoise)
            .put("p2pEnabled", peerToPeer)
            .put("regionLatency", JSONObject()))
        val grant = MobileVoiceGrant(
            json.getString("grant"), json.optString("roomId", roomId),
            json.optString("channelId", channelId), json.getString("host"),
            json.optInt("port", 25_565).coerceIn(1, 65_535),
            json.getString("certificateFingerprint"),
            json.optInt("bitrate", 24_000).coerceIn(12_000, 64_000),
            json.optBoolean("serverDenoise"), json.optBoolean("p2pEnabled"),
            json.optBoolean("canSpeak", true), json.optString("routeType", "relay"))
        require(grant.grant.length in 32..8192 && HOST.matches(grant.host)
            && FINGERPRINT.matches(grant.certificateFingerprint)) { "invalid_voice_grant" }
        return grant
    }

    fun bearerToken(): String {
        ensureAccessToken()
        return accessToken
    }

    private fun ensureAccessToken() {
        if (accessToken.isNotEmpty() && SystemClock.elapsedRealtime() + 60_000L < expiresAtElapsedMs) return
        synchronized(refreshLock) {
            if (accessToken.isNotEmpty() && SystemClock.elapsedRealtime() + 60_000L < expiresAtElapsedMs) return
            val refresh = secureStore.get(REFRESH_TOKEN)?.toString(StandardCharsets.UTF_8)
                ?: throw ApiException(401, "session_expired")
            refresh(refresh)
        }
    }

    private fun refresh(refreshToken: String) {
        val json = request("POST", "/api/v1/auth/refresh",
            JSONObject().put("refreshToken", refreshToken).put("clientVersion", "5.2.0"),
            authenticated = false)
        accessToken = json.getString("accessToken")
        expiresAtElapsedMs = SystemClock.elapsedRealtime() + json.optLong("expiresIn", 900L) * 1_000L
        val rotated = json.optString("refreshToken", refreshToken)
        secureStore.put(REFRESH_TOKEN, rotated.toByteArray(StandardCharsets.UTF_8))
    }

    private fun request(method: String, path: String, body: JSONObject? = null,
                        authenticated: Boolean = true, retry: Boolean = true): JSONObject {
        require(path.startsWith("/") && !path.startsWith("//")) { "invalid_path" }
        if (authenticated) ensureAccessToken()
        val connection = URL(base + path).openConnection() as HttpURLConnection
        try {
            connection.requestMethod = method
            connection.connectTimeout = 10_000
            connection.readTimeout = 10_000
            connection.instanceFollowRedirects = false
            connection.setRequestProperty("Accept", "application/json")
            connection.setRequestProperty("User-Agent", "SonalisMobile/5.2.0 Android")
            connection.setRequestProperty("X-Sonalis-Client", "native-mobile")
            if (authenticated) connection.setRequestProperty("Authorization", "Bearer $accessToken")
            if (body != null) {
                val bytes = body.toString().toByteArray(StandardCharsets.UTF_8)
                require(bytes.size <= 2 * 1024 * 1024) { "request_too_large" }
                connection.doOutput = true
                connection.setFixedLengthStreamingMode(bytes.size)
                connection.setRequestProperty("Content-Type", "application/json; charset=utf-8")
                connection.outputStream.use { it.write(bytes) }
                bytes.fill(0)
            }
            val status = connection.responseCode
            val response = readBounded(if (status in 200..299) connection.inputStream else connection.errorStream)
            if (status == 401 && authenticated && retry) {
                synchronized(refreshLock) {
                    expiresAtElapsedMs = 0
                    ensureAccessToken()
                }
                return request(method, path, body, authenticated, retry = false)
            }
            val json = if (response.isBlank()) JSONObject() else JSONObject(response)
            if (status !in 200..299) throw ApiException(status, json.optString("error", "request_failed"))
            return json
        } finally {
            connection.disconnect()
        }
    }

    private fun readBounded(stream: InputStream?): String {
        if (stream == null) return ""
        stream.use {
            val buffer = ByteArray(8_192)
            val output = java.io.ByteArrayOutputStream()
            while (true) {
                val read = it.read(buffer)
                if (read < 0) break
                if (output.size() + read > 2 * 1024 * 1024) throw ApiException(413, "response_too_large")
                output.write(buffer, 0, read)
            }
            return output.toString(StandardCharsets.UTF_8.name())
        }
    }

    private fun requireUuid(value: String) {
        require(UUID.matches(value)) { "invalid_uuid" }
    }

    companion object {
        private const val REFRESH_TOKEN = "refresh_token"
        private val UUID = Regex("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$")
        private val FINGERPRINT = Regex("^[0-9a-f]{64}$")
        private val HOST = Regex("^[A-Za-z0-9.-]{1,253}$")
    }
}

class ApiException(val status: Int, val safeCode: String) : RuntimeException(safeCode)
