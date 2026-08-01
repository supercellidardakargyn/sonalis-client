package tr.sonalis.mobile

import android.app.Activity
import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONObject
import tr.sonalis.core.NativeCore
import tr.sonalis.core.SonalisSecureStore
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

class MainActivity : Activity() {
    private lateinit var api: SonalisApi
    private lateinit var root: LinearLayout
    private val worker: ExecutorService = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    private val background = Color.rgb(9, 11, 18)
    private val surface = Color.rgb(28, 32, 45)
    private val primary = Color.rgb(124, 108, 242)
    private var destroyed = false
    private lateinit var voiceCall: AndroidVoiceCall
    private var pendingVoice: Triple<MobileRoom, List<MobileChannel>, MobileChannel>? = null
    private var realtime: SonalisRealtime? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        check(NativeCore.verify()) { "sonalis_core_abi_mismatch" }
        api = SonalisApi(SonalisSecureStore(this))
        voiceCall = AndroidVoiceCall(this, api) { value ->
            main.post { if (!destroyed && value == "audio_failed") showStatus("Ses cihazı bağlantısı kesildi.") }
        }
        root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(20), dp(20), dp(16))
            setBackgroundColor(this@MainActivity.background)
        }
        setContentView(root)
        showStatus("Güvenli oturum yükleniyor…")
        execute({ api.restore() }, { restored -> if (restored) loadRooms() else showLogin() })
    }

    private fun showLogin(message: String = "") {
        reset("Sonalis hesabına giriş")
        val login = input("Kullanıcı adı veya e-posta")
        val password = input("Parola", password = true)
        root.addView(login, match(dp(52), 8))
        root.addView(password, match(dp(52), 12))
        if (message.isNotEmpty()) root.addView(text(message, 13f, Color.rgb(244, 165, 96)), match(dp(40), 8))
        root.addView(action("Giriş yap") {
            val user = login.text.toString().trim()
            val secret = password.text.toString()
            if (user.isEmpty() || secret.isEmpty()) return@action
            password.text.clear()
            showStatus("Oturum doğrulanıyor…")
            execute({ api.login(user, secret) }, { loadRooms() }, { showLogin(safeError(it)) })
        }, match(dp(52), 12))
        root.addView(text("Parola saklanmaz. Yenileme anahtarı Android Keystore ile korunur.",
            13f, Color.rgb(154, 163, 185)), match(0, 0, 1f))
    }

    private fun loadRooms() {
        showStatus("Odalar yükleniyor…")
        execute({ api.rooms() }, {
            ensureRealtime()
            showRooms(it)
        }, { showLogin("Oturum yenilenemedi.") })
    }

    private fun ensureRealtime() {
        if (realtime != null) return
        realtime = SonalisRealtime(api, { event ->
            val type = runCatching { JSONObject(event).optString("type") }.getOrDefault("")
            if (type == "session.revoked") main.post {
                if (!destroyed) { voiceCall.close(); realtime?.close(); realtime = null; showLogin("Oturum sona erdi.") }
            }
        }, { }).also { it.connect() }
    }

    private fun showRooms(rooms: List<MobileRoom>) {
        reset("Odalar")
        root.addView(text("Üyesi olduğunuz topluluklar", 13f, Color.rgb(154, 163, 185)), match(dp(36), 10))
        val list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        rooms.forEach { room ->
            list.addView(action("${room.name}  ·  ${room.role}") {
                showStatus("Kanallar yükleniyor…")
                execute({ api.channels(room.id) }, { showChannels(room, it) }, { showRooms(rooms) })
            }, match(dp(54), 8))
        }
        if (rooms.isEmpty()) list.addView(text("Henüz üyesi olduğunuz bir oda yok.", 14f, Color.LTGRAY), match(dp(52), 8))
        root.addView(ScrollView(this).apply { addView(list) }, match(0, 8, 1f))
        root.addView(action("Oturumu kapat", primaryAction = false) {
            voiceCall.close()
            realtime?.close(); realtime = null
            execute({ api.logout() }, { showLogin() })
        }, match(dp(48), 0))
    }

    private fun showChannels(room: MobileRoom, channels: List<MobileChannel>) {
        reset(room.name, back = { loadRooms() })
        val list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        channels.forEach { channel ->
            val prefix = if (channel.type == "voice") "Ses" else "#"
            val badge = if (channel.mentionCount > 0) "  @${channel.mentionCount}" else if (channel.unreadCount > 0) "  ${channel.unreadCount}" else ""
            list.addView(action("$prefix  ${channel.name}$badge", primaryAction = false) {
                if (channel.type == "text") {
                    showStatus("Mesajlar yükleniyor…")
                    execute({ api.messages(channel.id) }, { showMessages(room, channels, channel, it) },
                        { showChannels(room, channels) })
                } else {
                    requestVoice(room, channels, channel)
                }
            }, match(dp(52), 8))
        }
        root.addView(ScrollView(this).apply { addView(list) }, match(0, 0, 1f))
    }

    private fun requestVoice(room: MobileRoom, channels: List<MobileChannel>, channel: MobileChannel) {
        if (Build.VERSION.SDK_INT >= 23 && checkSelfPermission(Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
            pendingVoice = Triple(room, channels, channel)
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), MICROPHONE_REQUEST)
            return
        }
        connectVoice(room, channels, channel, capture = true)
    }

    private fun connectVoice(room: MobileRoom, channels: List<MobileChannel>, channel: MobileChannel,
                             capture: Boolean) {
        showStatus(if (capture) "Ses kanalına bağlanılıyor…" else "Dinleme modunda bağlanılıyor…")
        execute({ voiceCall.connect(room.id, channel.id, capture) }, {
            showChannels(room, channels)
            root.addView(text("${channel.name} · ${if (capture) "Bağlı" else "Yalnız dinleme"}", 13f,
                Color.rgb(83, 214, 166)), match(dp(38), 8))
            root.addView(action("Ses kanalından ayrıl", primaryAction = false) {
                voiceCall.close(); showChannels(room, channels)
            }, match(dp(48), 0))
        }, { showChannels(room, channels) })
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>,
                                            grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != MICROPHONE_REQUEST) return
        val pending = pendingVoice ?: return
        pendingVoice = null
        connectVoice(pending.first, pending.second, pending.third,
            capture = grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED)
    }

    private fun showMessages(room: MobileRoom, channels: List<MobileChannel>, channel: MobileChannel,
                             messages: List<MobileMessage>) {
        reset("# ${channel.name}", back = { showChannels(room, channels) })
        val list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        messages.forEach { message ->
            list.addView(text("${message.senderId.take(8)}  ·  ${message.createdAt}\nŞifreli mesaj",
                13f, Color.rgb(210, 214, 226)).apply {
                setPadding(dp(14), dp(10), dp(14), dp(10)); setBackgroundColor(surface)
            }, match(dp(64), 6))
        }
        if (messages.isEmpty()) list.addView(text("Bu kanalda henüz mesaj yok.", 14f, Color.LTGRAY), match(dp(52), 8))
        root.addView(ScrollView(this).apply { addView(list) }, match(0, 0, 1f))
        root.addView(text("Mesaj içeriği yalnız cihazdaki E2EE anahtarıyla açılır.", 12f,
            Color.rgb(154, 163, 185)), match(dp(42), 0))
    }

    private fun reset(title: String, back: (() -> Unit)? = null) {
        root.removeAllViews()
        val header = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        if (back != null) header.addView(action("‹", primaryAction = false) { back() }, LinearLayout.LayoutParams(dp(48), dp(46)))
        header.addView(text(title, 21f, Color.WHITE), LinearLayout.LayoutParams(0, dp(52), 1f))
        root.addView(header, match(dp(56), 12))
    }

    private fun showStatus(value: String) {
        root.removeAllViews()
        root.addView(text("SONALIS", 22f, Color.WHITE), match(dp(54), 10))
        root.addView(text(value, 14f, Color.rgb(180, 187, 205)).apply { gravity = Gravity.CENTER }, match(0, 0, 1f))
    }

    private fun input(hint: String, password: Boolean = false) = EditText(this).apply {
        this.hint = hint
        setHintTextColor(Color.rgb(126, 135, 156))
        setTextColor(Color.WHITE)
        setSingleLine(true)
        setPadding(dp(14), 0, dp(14), 0)
        setBackgroundColor(surface)
        if (password) inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
    }

    private fun action(label: String, primaryAction: Boolean = true, callback: () -> Unit) = Button(this).apply {
        text = label
        isAllCaps = false
        setTextColor(Color.WHITE)
        setBackgroundColor(if (primaryAction) primary else surface)
        setOnClickListener { callback() }
    }

    private fun text(value: String, size: Float, color: Int) = TextView(this).apply {
        text = value
        textSize = size
        setTextColor(color)
        gravity = Gravity.CENTER_VERTICAL
    }

    private fun match(height: Int, bottom: Int, weight: Float = 0f) =
        LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, if (weight > 0) 0 else height, weight).apply {
            bottomMargin = dp(bottom)
        }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()

    private fun <T> execute(work: () -> T, success: (T) -> Unit, failure: (Throwable) -> Unit = { showStatus(safeError(it)) }) {
        worker.execute {
            try {
                val result = work()
                main.post { if (!destroyed) success(result) }
            } catch (failureValue: Throwable) {
                main.post { if (!destroyed) failure(failureValue) }
            }
        }
    }

    private fun safeError(error: Throwable) = when (error) {
        is ApiException -> when (error.safeCode) {
            "invalid_credentials" -> "Kullanıcı adı veya parola yanlış."
            "account_not_active" -> "Hesap henüz etkin değil."
            "session_expired", "refresh_invalid" -> "Oturum sona erdi."
            else -> "İşlem tamamlanamadı (${error.status})."
        }
        else -> "Bağlantı kurulamadı."
    }

    override fun onDestroy() {
        destroyed = true
        voiceCall.close()
        realtime?.close(); realtime = null
        worker.shutdownNow()
        super.onDestroy()
    }

    private companion object { const val MICROPHONE_REQUEST = 7001 }
}
