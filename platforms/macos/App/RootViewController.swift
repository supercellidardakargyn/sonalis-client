import AppKit
import AVFoundation

@MainActor
final class RootViewController: NSViewController {
    private let api = SonalisAPI(secureStore: SonalisKeychainStore())
    private let content = NSStackView()
    private let status = NSTextField(labelWithString: "")
    private var rooms: [AppleRoom] = []
    private var selectedRoom: AppleRoom?
    private var channels: [AppleChannel] = []
    private var voiceCall: SonalisVoiceCall?

    override func loadView() {
        let root = NSView()
        root.wantsLayer = true
        root.layer?.backgroundColor = NSColor(calibratedRed: 0.035, green: 0.043, blue: 0.071, alpha: 1).cgColor
        content.orientation = .vertical
        content.spacing = 12
        content.alignment = .leading
        content.translatesAutoresizingMaskIntoConstraints = false
        status.textColor = NSColor(calibratedRed: 0.62, green: 0.66, blue: 0.77, alpha: 1)
        status.maximumNumberOfLines = 2
        root.addSubview(content)
        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 24),
            content.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -24),
            content.topAnchor.constraint(equalTo: root.topAnchor, constant: 24),
            content.bottomAnchor.constraint(lessThanOrEqualTo: root.bottomAnchor, constant: -20),
        ])
        self.view = root
        showLoading("Güvenli oturum yükleniyor…")
        Task { if await api.restore() { await loadRooms() } else { showLogin() } }
    }

    private func reset(_ title: String, back: (() -> Void)? = nil) {
        content.arrangedSubviews.forEach { content.removeArrangedSubview($0); $0.removeFromSuperview() }
        let header = NSStackView()
        header.orientation = .horizontal
        header.spacing = 10
        if let back { header.addArrangedSubview(button("‹", primary: false, width: 46, action: back)) }
        header.addArrangedSubview(label(title, size: 22, weight: .bold, color: .white))
        content.addArrangedSubview(header)
        status.stringValue = ""
        content.addArrangedSubview(status)
    }

    private func showLoading(_ message: String) { reset("SONALIS"); status.stringValue = message }

    private func showLogin(_ message: String = "") {
        reset("Sonalis hesabına giriş")
        let login = NSTextField(string: "")
        login.placeholderString = "Kullanıcı adı veya e-posta"
        let password = NSSecureTextField(string: "")
        password.placeholderString = "Parola"
        for field in [login, password] {
            field.widthAnchor.constraint(greaterThanOrEqualToConstant: 420).isActive = true
            field.heightAnchor.constraint(equalToConstant: 38).isActive = true
            content.addArrangedSubview(field)
        }
        if !message.isEmpty { status.stringValue = message; status.textColor = .systemOrange }
        content.addArrangedSubview(button("Giriş yap") { [weak self] in
            guard let self, !login.stringValue.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
                  !password.stringValue.isEmpty else { return }
            let secret = password.stringValue
            password.stringValue = ""
            self.showLoading("Oturum doğrulanıyor…")
            Task {
                do {
                    _ = try await self.api.login(login.stringValue, password: secret,
                                                 deviceName: Host.current().localizedName ?? "Mac")
                    await self.loadRooms()
                } catch { self.showLogin(self.safe(error)) }
            }
        })
        content.addArrangedSubview(label("Parola saklanmaz; yenileme anahtarı ThisDeviceOnly Keychain girdisidir.",
                                         size: 13, color: .secondaryLabelColor))
    }

    private func loadRooms() async {
        showLoading("Odalar yükleniyor…")
        do { rooms = try await api.rooms(); showRooms() }
        catch { showLogin(safe(error)) }
    }

    private func showRooms() {
        reset("Odalar")
        if rooms.isEmpty { content.addArrangedSubview(label("Henüz üyesi olduğunuz oda yok.", size: 14)) }
        rooms.forEach { room in
            content.addArrangedSubview(button("\(room.name)  ·  \(room.role)", primary: false) { [weak self] in
                guard let self else { return }
                self.selectedRoom = room
                self.showLoading("Kanallar yükleniyor…")
                Task {
                    do { self.channels = try await self.api.roomOverview(room.id).channels; self.showChannels() }
                    catch { self.status.stringValue = self.safe(error) }
                }
            })
        }
        content.addArrangedSubview(button("Oturumu kapat", primary: false) { [weak self] in
            guard let self else { return }
            Task { await self.api.logout(); self.showLogin() }
        })
    }

    private func showChannels() {
        guard let room = selectedRoom else { showRooms(); return }
        reset(room.name, back: { [weak self] in self?.showRooms() })
        channels.forEach { channel in
            let prefix = channel.type == "voice" ? "Ses" : "#"
            content.addArrangedSubview(button("\(prefix)  \(channel.name)", primary: false) { [weak self] in
                guard let self else { return }
                if channel.type == "text" {
                    self.showLoading("Şifreli mesajlar yükleniyor…")
                    Task {
                        do { self.showMessages(channel, try await self.api.messages(channelId: channel.id).messages) }
                        catch { self.status.stringValue = self.safe(error) }
                    }
                } else { self.connectVoice(room: room, channel: channel) }
            })
        }
        if voiceCall != nil {
            content.addArrangedSubview(button("Ses kanalından ayrıl", primary: false) { [weak self] in
                self?.voiceCall?.close()
                self?.voiceCall = nil
                self?.showChannels()
            })
        }
    }

    private func connectVoice(room: AppleRoom, channel: AppleChannel) {
        showLoading("Ses kanalına bağlanıyor…")
        Task {
            let microphone = await AVCaptureDevice.requestAccess(for: .audio)
            do {
                let grant = try await api.voiceGrant(roomId: room.id, channelId: channel.id,
                                                     serverDenoise: false, peerToPeer: false)
                let effective = AppleVoiceGrant(grant: grant.grant, roomId: grant.roomId,
                    channelId: grant.channelId, host: grant.host, port: grant.port,
                    certificateFingerprint: grant.certificateFingerprint,
                    serverDenoise: grant.serverDenoise, p2pEnabled: false,
                    canSpeak: grant.canSpeak && microphone, bitrate: grant.bitrate,
                    routeType: grant.routeType)
                let call = SonalisVoiceCall { [weak self] state in
                    Task { @MainActor in self?.status.stringValue = state }
                }
                try await call.connect(effective)
                voiceCall?.close()
                voiceCall = call
                showChannels()
                status.stringValue = microphone ? "Bağlandı · şifreli relay" : "Bağlandı · yalnız dinleme"
            } catch { showChannels(); status.stringValue = safe(error) }
        }
    }

    private func showMessages(_ channel: AppleChannel, _ messages: [AppleEncryptedMessage]) {
        reset("# \(channel.name)", back: { [weak self] in self?.showChannels() })
        if messages.isEmpty { content.addArrangedSubview(label("Bu kanalda henüz mesaj yok.", size: 14)) }
        messages.suffix(50).forEach { message in
            content.addArrangedSubview(label("\(message.senderId.prefix(8)) · \(message.createdAt) · Şifreli mesaj",
                                             size: 13, color: .labelColor))
        }
    }

    private func label(_ text: String, size: CGFloat, weight: NSFont.Weight = .regular,
                       color: NSColor = .secondaryLabelColor) -> NSTextField {
        let value = NSTextField(wrappingLabelWithString: text)
        value.font = .systemFont(ofSize: size, weight: weight)
        value.textColor = color
        return value
    }

    private func button(_ title: String, primary: Bool = true, width: CGFloat = 420,
                        action: @escaping () -> Void) -> NSButton {
        let value = ClosureButton(title: title, action: action)
        value.bezelStyle = primary ? .rounded : .recessed
        value.widthAnchor.constraint(greaterThanOrEqualToConstant: width).isActive = true
        value.heightAnchor.constraint(equalToConstant: 40).isActive = true
        return value
    }

    private func safe(_ error: Error) -> String {
        guard let apiError = error as? SonalisAPIError else { return "Bağlantı kurulamadı." }
        switch apiError.safeCode {
        case "invalid_credentials": return "Kullanıcı adı veya parola yanlış."
        case "account_not_active": return "Hesap henüz etkin değil."
        case "session_expired", "refresh_invalid": return "Oturum sona erdi."
        default: return "İşlem tamamlanamadı (\(apiError.status))."
        }
    }
}

private final class ClosureButton: NSButton {
    private let callback: () -> Void
    init(title: String, action: @escaping () -> Void) {
        self.callback = action
        super.init(frame: .zero)
        self.title = title
        target = self
        self.action = #selector(invoke)
    }
    required init?(coder: NSCoder) { nil }
    @objc private func invoke() { callback() }
}
