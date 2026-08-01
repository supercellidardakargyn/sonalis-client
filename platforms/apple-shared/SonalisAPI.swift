import Foundation

protocol SecureTokenStore {
    func put(_ value: Data, key: String) throws
    func get(_ key: String) throws -> Data?
    func erase(_ key: String) throws
}

struct AppleUser: Codable { let id: String; let username: String; let nickname: String }
struct AppleRoom: Codable { let id: String; let name: String; let role: String }
struct AppleRoomsResponse: Codable { let rooms: [AppleRoom] }
struct AppleChannel: Codable {
    let id: String
    let categoryId: String?
    let type: String
    let name: String
    let topic: String?
    let unreadCount: UInt32?
    let mentionCount: UInt32?
}
struct AppleRoomOverview: Codable { let room: AppleRoom; let channels: [AppleChannel] }
struct AppleEncryptedMessage: Codable {
    let id: String
    let channelId: String?
    let senderId: String
    let ciphertext: String
    let nonce: String
    let signature: String
    let createdAt: String
}
struct AppleMessagesResponse: Codable {
    let messages: [AppleEncryptedMessage]
    let beforeCursor: String?
    let afterCursor: String?
    let hasMore: Bool?
}
struct AppleRealtimeGrant: Codable { let ticket: String; let expiresAt: String; let websocketUrl: String }
struct AppleVoiceGrant: Codable {
    let grant: String
    let roomId: String
    let channelId: String
    let host: String
    let port: UInt16
    let certificateFingerprint: String
    let serverDenoise: Bool
    let p2pEnabled: Bool
    let canSpeak: Bool
    let bitrate: UInt32
    let routeType: String
}

private struct LoginResponse: Codable {
    let user: AppleUser
    let accessToken: String
    let refreshToken: String
    let expiresIn: Int
}
private struct RefreshResponse: Codable {
    let accessToken: String
    let refreshToken: String?
    let expiresIn: Int
}
private struct APIErrorResponse: Codable { let error: String? }

struct SonalisAPIError: Error {
    let status: Int
    let safeCode: String
}

actor SonalisAPI {
    private static let refreshKey = "refresh_token"
    private let origin: URL
    private let secureStore: SecureTokenStore
    private let session: URLSession
    private var accessToken = ""
    private var accessExpiresAt = Date.distantPast
    private var user: AppleUser?

    init(origin: URL = URL(string: "https://sonalis.tr")!, secureStore: SecureTokenStore) {
        precondition(origin.scheme == "https" && origin.host != nil && origin.user == nil && origin.password == nil)
        self.origin = origin
        self.secureStore = secureStore
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 10
        configuration.timeoutIntervalForResource = 20
        configuration.httpMaximumConnectionsPerHost = 4
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.httpAdditionalHeaders = [
            "Accept": "application/json",
            "User-Agent": "SonalisApple/5.2.0",
            "X-Sonalis-Client": "native-mobile",
        ]
        self.session = URLSession(configuration: configuration)
    }

    func login(_ login: String, password: String, deviceName: String) async throws -> AppleUser {
        let payload: [String: Any] = [
            "login": login.trimmingCharacters(in: .whitespacesAndNewlines),
            "password": password,
            "deviceName": String(deviceName.prefix(120)),
            "clientVersion": "5.2.0",
        ]
        let response: LoginResponse = try await send("POST", "/api/v1/auth/login", payload,
                                                     authenticated: false, retry: false)
        accessToken = response.accessToken
        accessExpiresAt = Date().addingTimeInterval(TimeInterval(response.expiresIn))
        user = response.user
        try secureStore.put(Data(response.refreshToken.utf8), key: Self.refreshKey)
        return response.user
    }

    func restore() async -> Bool {
        do {
            guard let data = try secureStore.get(Self.refreshKey),
                  let refreshToken = String(data: data, encoding: .utf8), !refreshToken.isEmpty else { return false }
            try await refresh(refreshToken)
            return true
        } catch {
            return false
        }
    }

    func logout() async {
        let refreshToken = (try? secureStore.get(Self.refreshKey)).flatMap { String(data: $0, encoding: .utf8) }
        if let refreshToken {
            let payload: [String: Any] = ["refreshToken": refreshToken]
            let _: EmptyResponse? = try? await send("POST", "/api/v1/auth/logout", payload,
                                                    authenticated: false, retry: false)
        }
        accessToken = ""
        accessExpiresAt = .distantPast
        user = nil
        try? secureStore.erase(Self.refreshKey)
    }

    func rooms() async throws -> [AppleRoom] {
        let response: AppleRoomsResponse = try await send("GET", "/api/v1/me/rooms")
        return Array(response.rooms.prefix(100))
    }

    func roomOverview(_ roomId: String) async throws -> AppleRoomOverview {
        try requireUUID(roomId)
        let response: AppleRoomOverview = try await send("GET", "/api/v1/rooms/\(roomId)/overview")
        return AppleRoomOverview(room: response.room, channels: Array(response.channels.prefix(100)))
    }

    func messages(channelId: String, beforeCursor: String? = nil) async throws -> AppleMessagesResponse {
        try requireUUID(channelId)
        var components = URLComponents()
        components.queryItems = [URLQueryItem(name: "limit", value: "50")]
        if let beforeCursor, beforeCursor.utf8.count <= 256 {
            components.queryItems?.append(URLQueryItem(name: "beforeCursor", value: beforeCursor))
        }
        let response: AppleMessagesResponse = try await send(
            "GET", "/api/v1/channels/\(channelId)/messages?\(components.percentEncodedQuery ?? "limit=50")")
        return AppleMessagesResponse(messages: Array(response.messages.prefix(50)),
                                     beforeCursor: response.beforeCursor,
                                     afterCursor: response.afterCursor, hasMore: response.hasMore)
    }

    func realtimeGrant() async throws -> AppleRealtimeGrant {
        let grant: AppleRealtimeGrant = try await send("POST", "/api/v1/realtime/grant", [:])
        guard let url = URL(string: grant.websocketUrl), url.scheme == "wss", url.host == origin.host,
              grant.ticket.utf8.count >= 32, grant.ticket.utf8.count <= 128 else {
            throw SonalisAPIError(status: 0, safeCode: "invalid_realtime_grant")
        }
        return grant
    }

    func voiceGrant(roomId: String, channelId: String, serverDenoise: Bool,
                    peerToPeer: Bool) async throws -> AppleVoiceGrant {
        try requireUUID(roomId)
        try requireUUID(channelId)
        let payload: [String: Any] = [
            "roomId": roomId,
            "channelId": channelId,
            "serverDenoiseRequested": serverDenoise,
            "p2pEnabled": peerToPeer,
            "regionLatency": [String: Double](),
        ]
        let grant: AppleVoiceGrant = try await send("POST", "/api/v1/voice/join-grant", payload)
        let fingerprint = grant.certificateFingerprint.utf8
        guard grant.grant.utf8.count >= 32, grant.grant.utf8.count <= 8_192,
              !grant.host.isEmpty, grant.port > 0, grant.bitrate >= 12_000, grant.bitrate <= 64_000,
              fingerprint.count == 64, fingerprint.allSatisfy({
                  ($0 >= 48 && $0 <= 57) || ($0 >= 97 && $0 <= 102)
              }) else {
            throw SonalisAPIError(status: 0, safeCode: "invalid_voice_grant")
        }
        return grant
    }

    private func ensureAccess() async throws {
        if !accessToken.isEmpty && accessExpiresAt.timeIntervalSinceNow > 60 { return }
        guard let data = try secureStore.get(Self.refreshKey),
              let token = String(data: data, encoding: .utf8), !token.isEmpty else {
            throw SonalisAPIError(status: 401, safeCode: "session_expired")
        }
        try await refresh(token)
    }

    private func refresh(_ token: String) async throws {
        let payload: [String: Any] = ["refreshToken": token, "clientVersion": "5.2.0"]
        let response: RefreshResponse = try await send("POST", "/api/v1/auth/refresh", payload,
                                                       authenticated: false, retry: false)
        accessToken = response.accessToken
        accessExpiresAt = Date().addingTimeInterval(TimeInterval(response.expiresIn))
        try secureStore.put(Data((response.refreshToken ?? token).utf8), key: Self.refreshKey)
    }

    private func send<T: Decodable>(_ method: String, _ path: String,
                                    _ body: [String: Any]? = nil,
                                    authenticated: Bool = true, retry: Bool = true) async throws -> T {
        guard path.hasPrefix("/"), !path.hasPrefix("//"),
              let target = URL(string: path, relativeTo: origin)?.absoluteURL,
              target.scheme == "https", target.host == origin.host else {
            throw SonalisAPIError(status: 0, safeCode: "invalid_request_path")
        }
        if authenticated { try await ensureAccess() }
        var request = URLRequest(url: target)
        request.httpMethod = method
        if authenticated { request.setValue("Bearer \(accessToken)", forHTTPHeaderField: "Authorization") }
        if let body {
            let data = try JSONSerialization.data(withJSONObject: body)
            guard data.count <= 2 * 1024 * 1024 else {
                throw SonalisAPIError(status: 413, safeCode: "request_too_large")
            }
            request.httpBody = data
            request.setValue("application/json; charset=utf-8", forHTTPHeaderField: "Content-Type")
        }
        let (data, response) = try await session.data(for: request)
        guard data.count <= 2 * 1024 * 1024, let http = response as? HTTPURLResponse else {
            throw SonalisAPIError(status: 0, safeCode: "response_invalid")
        }
        if http.statusCode == 401 && authenticated && retry {
            accessExpiresAt = .distantPast
            try await ensureAccess()
            return try await send(method, path, body, authenticated: true, retry: false)
        }
        guard (200...299).contains(http.statusCode) else {
            let code = (try? JSONDecoder().decode(APIErrorResponse.self, from: data).error) ?? "request_failed"
            throw SonalisAPIError(status: http.statusCode, safeCode: code)
        }
        if T.self == EmptyResponse.self && data.isEmpty { return EmptyResponse() as! T }
        return try JSONDecoder().decode(T.self, from: data)
    }

    private func requireUUID(_ value: String) throws {
        guard UUID(uuidString: value) != nil, value.utf8.count == 36 else {
            throw SonalisAPIError(status: 0, safeCode: "invalid_uuid")
        }
    }
}

private struct EmptyResponse: Codable {}
