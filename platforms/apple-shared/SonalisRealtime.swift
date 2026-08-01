import Foundation

@MainActor
final class SonalisRealtime {
    private let api: SonalisAPI
    private var session: URLSession?
    private var socket: URLSessionWebSocketTask?
    private var receiveTask: Task<Void, Never>?
    private var pingTask: Task<Void, Never>?
    private(set) var connected = false
    var onEvent: ((String) -> Void)?
    var onState: ((String) -> Void)?

    init(api: SonalisAPI) { self.api = api }

    func connect() {
        disconnect()
        receiveTask = Task { [weak self] in
            guard let self else { return }
            do {
                let grant = try await api.realtimeGrant()
                guard var components = URLComponents(string: grant.websocketUrl) else { throw URLError(.badURL) }
                components.queryItems = [URLQueryItem(name: "ticket", value: grant.ticket)]
                guard let target = components.url else { throw URLError(.badURL) }
                let configuration = URLSessionConfiguration.ephemeral
                configuration.timeoutIntervalForRequest = 10
                let session = URLSession(configuration: configuration)
                self.session = session
                let socket = session.webSocketTask(with: target)
                self.socket = socket
                socket.resume()
                self.connected = true
                self.onState?("connected")
                self.pingTask = Task { [weak self, weak socket] in
                    while !Task.isCancelled, let self, let socket {
                        try? await Task.sleep(nanoseconds: 25_000_000_000)
                        if Task.isCancelled { return }
                        do { try await self.ping(socket) }
                        catch { socket.cancel(with: .goingAway, reason: nil); return }
                    }
                }
                while !Task.isCancelled {
                    let message = try await socket.receive()
                    switch message {
                    case .string(let text) where text.utf8.count <= 32 * 1024: self.onEvent?(text)
                    case .data(let data) where data.count <= 32 * 1024:
                        if let text = String(data: data, encoding: .utf8) { self.onEvent?(text) }
                    default: break
                    }
                }
            } catch {
                self.connected = false
                self.onState?("offline")
            }
        }
    }

    func disconnect() {
        pingTask?.cancel()
        pingTask = nil
        receiveTask?.cancel()
        receiveTask = nil
        socket?.cancel(with: .normalClosure, reason: nil)
        socket = nil
        session?.invalidateAndCancel()
        session = nil
        connected = false
    }

    private func ping(_ socket: URLSessionWebSocketTask) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            socket.sendPing { error in
                if let error { continuation.resume(throwing: error) }
                else { continuation.resume() }
            }
        }
    }
}
