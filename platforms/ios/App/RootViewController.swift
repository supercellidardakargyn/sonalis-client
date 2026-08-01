import AVFoundation
import UIKit

@MainActor
final class RootViewController: UIViewController {
    private let api = SonalisAPI(secureStore: SonalisKeychainStore())
    private let content = UIStackView()
    private let status = UILabel()
    private var rooms: [AppleRoom] = []
    private var channels: [AppleChannel] = []
    private var selectedRoom: AppleRoom?
    private var voiceCall: SonalisVoiceCall?

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = UIColor(red: 0.035, green: 0.043, blue: 0.071, alpha: 1)
        content.axis = .vertical
        content.spacing = 12
        content.translatesAutoresizingMaskIntoConstraints = false
        status.font = .systemFont(ofSize: 13, weight: .medium)
        status.textColor = UIColor(red: 0.62, green: 0.66, blue: 0.77, alpha: 1)
        status.numberOfLines = 2
        view.addSubview(content)
        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor, constant: 20),
            content.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -20),
            content.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 20),
            content.bottomAnchor.constraint(lessThanOrEqualTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -16),
        ])
        showLoading("Güvenli oturum yükleniyor…")
        Task { if await api.restore() { await loadRooms() } else { showLogin() } }
    }

    private func reset(title: String, back: (() -> Void)? = nil) {
        content.arrangedSubviews.forEach { $0.removeFromSuperview() }
        let header = UIStackView()
        header.axis = .horizontal
        header.spacing = 10
        if let back { header.addArrangedSubview(button("‹", primary: false, action: back, width: 46)) }
        let titleLabel = label(title, size: 22, weight: .bold, color: .white)
        header.addArrangedSubview(titleLabel)
        content.addArrangedSubview(header)
        content.addArrangedSubview(status)
        status.text = ""
    }

    private func showLoading(_ message: String) {
        reset(title: "SONALIS")
        status.text = message
    }

    private func showLogin(message: String = "") {
        reset(title: "Sonalis hesabına giriş")
        let login = field("Kullanıcı adı veya e-posta")
        let password = field("Parola", secure: true)
        content.addArrangedSubview(login)
        content.addArrangedSubview(password)
        if !message.isEmpty { status.text = message; status.textColor = .systemOrange }
        content.addArrangedSubview(button("Giriş yap") { [weak self] in
            guard let self, let username = login.text, let secret = password.text,
                  !username.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, !secret.isEmpty else { return }
            password.text = ""
            self.showLoading("Oturum doğrulanıyor…")
            Task {
                do {
                    _ = try await self.api.login(username, password: secret, deviceName: UIDevice.current.name)
                    await self.loadRooms()
                } catch { self.showLogin(message: self.safe(error)) }
            }
        })
        content.addArrangedSubview(label("Parola saklanmaz. Yenileme anahtarı yalnız bu cihazdaki Keychain'de tutulur.",
                                         size: 13, color: status.textColor))
    }

    private func loadRooms() async {
        showLoading("Odalar yükleniyor…")
        do { rooms = try await api.rooms(); showRooms() }
        catch { showLogin(message: safe(error)) }
    }

    private func showRooms() {
        reset(title: "Odalar")
        content.addArrangedSubview(label("Üyesi olduğunuz topluluklar", size: 13, color: status.textColor))
        if rooms.isEmpty { content.addArrangedSubview(label("Henüz üyesi olduğunuz bir oda yok.", size: 14)) }
        rooms.forEach { room in
            content.addArrangedSubview(button("\(room.name)  ·  \(room.role)", primary: false) { [weak self] in
                guard let self else { return }
                self.selectedRoom = room
                self.showLoading("Kanallar yükleniyor…")
                Task {
                    do { self.channels = try await self.api.roomOverview(room.id).channels; self.showChannels() }
                    catch { self.status.text = self.safe(error) }
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
        reset(title: room.name, back: { [weak self] in self?.showRooms() })
        channels.forEach { channel in
            let prefix = channel.type == "voice" ? "Ses" : "#"
            let badge = (channel.mentionCount ?? 0) > 0 ? "  @\(channel.mentionCount ?? 0)" : ""
            content.addArrangedSubview(button("\(prefix)  \(channel.name)\(badge)", primary: false) { [weak self] in
                guard let self else { return }
                if channel.type == "text" {
                    self.showLoading("Şifreli mesajlar yükleniyor…")
                    Task {
                        do { self.showMessages(channel, try await self.api.messages(channelId: channel.id).messages) }
                        catch { self.status.text = self.safe(error) }
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
            let microphone = await withCheckedContinuation { continuation in
                AVAudioApplication.requestRecordPermission { continuation.resume(returning: $0) }
            }
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
                    Task { @MainActor in self?.status.text = state }
                }
                try await call.connect(effective)
                voiceCall?.close()
                voiceCall = call
                showChannels()
                status.text = microphone ? "Bağlandı · şifreli relay" : "Bağlandı · yalnız dinleme"
            } catch { showChannels(); status.text = safe(error) }
        }
    }

    private func showMessages(_ channel: AppleChannel, _ messages: [AppleEncryptedMessage]) {
        reset(title: "# \(channel.name)", back: { [weak self] in self?.showChannels() })
        if messages.isEmpty { content.addArrangedSubview(label("Bu kanalda henüz mesaj yok.", size: 14)) }
        messages.suffix(50).forEach { message in
            content.addArrangedSubview(label("\(message.senderId.prefix(8))  ·  \(message.createdAt)\nŞifreli mesaj",
                                                  size: 13, color: .white))
        }
        content.addArrangedSubview(label("İçerik yalnız cihazdaki E2EE anahtarıyla açılır.", size: 12,
                                         color: status.textColor))
    }

    private func field(_ placeholder: String, secure: Bool = false) -> UITextField {
        let value = UITextField()
        value.placeholder = placeholder
        value.isSecureTextEntry = secure
        value.textColor = .white
        value.backgroundColor = UIColor(red: 0.13, green: 0.15, blue: 0.22, alpha: 1)
        value.layer.cornerRadius = 10
        value.setLeftPadding(14)
        value.heightAnchor.constraint(equalToConstant: 52).isActive = true
        value.autocapitalizationType = .none
        value.autocorrectionType = .no
        return value
    }

    private func button(_ title: String, primary: Bool = true, action: @escaping () -> Void,
                        width: CGFloat? = nil) -> UIButton {
        var configuration = UIButton.Configuration.filled()
        configuration.title = title
        configuration.baseBackgroundColor = primary ? UIColor(red: 0.49, green: 0.42, blue: 0.95, alpha: 1)
                                                    : UIColor(red: 0.13, green: 0.15, blue: 0.22, alpha: 1)
        configuration.baseForegroundColor = .white
        configuration.cornerStyle = .medium
        let value = UIButton(configuration: configuration, primaryAction: UIAction { _ in action() })
        value.heightAnchor.constraint(equalToConstant: 50).isActive = true
        if let width { value.widthAnchor.constraint(equalToConstant: width).isActive = true }
        return value
    }

    private func label(_ text: String, size: CGFloat, weight: UIFont.Weight = .regular,
                       color: UIColor = .lightGray) -> UILabel {
        let value = UILabel()
        value.text = text
        value.font = .systemFont(ofSize: size, weight: weight)
        value.textColor = color
        value.numberOfLines = 0
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

private extension UITextField {
    func setLeftPadding(_ amount: CGFloat) {
        let spacer = UIView(frame: CGRect(x: 0, y: 0, width: amount, height: 1))
        leftView = spacer
        leftViewMode = .always
    }
}
