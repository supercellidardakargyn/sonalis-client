import CryptoKit
import Foundation
import Network
import Security

struct AppleVoiceFrame {
    let senderId: String
    let sequence: UInt16
    let timestamp: UInt32
    let flags: UInt8
    let opus: Data
}

enum AppleVoiceTransportError: Error {
    case invalidGrant, certificateMismatch, joinRejected, invalidSession, udpUnavailable, bindTimeout
}

private final class VoicePinDelegate: NSObject, URLSessionDelegate {
    let expected: Data
    init(expected: Data) { self.expected = expected }

    func urlSession(_ session: URLSession, didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        guard challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
              let trust = challenge.protectionSpace.serverTrust,
              let certificate = SecTrustGetCertificateAtIndex(trust, 0) else {
            completionHandler(.cancelAuthenticationChallenge, nil)
            return
        }
        let digest = Data(SHA256.hash(data: SecCertificateCopyData(certificate) as Data))
        guard digest == expected else {
            completionHandler(.cancelAuthenticationChallenge, nil)
            return
        }
        completionHandler(.useCredential, URLCredential(trust: trust))
    }
}

final class SonalisVoiceTransport: @unchecked Sendable {
    typealias FrameHandler = @Sendable (AppleVoiceFrame) -> Void
    typealias StateHandler = @Sendable (String) -> Void

    private let queue = DispatchQueue(label: "tr.sonalis.voice.transport", qos: .userInteractive)
    private let frameHandler: FrameHandler
    private let stateHandler: StateHandler
    private var connection: NWConnection?
    private var sessionId = Data()
    private var key = Data()
    private var noncePrefix = Data()
    private var outgoing: UInt64 = 0
    private var highestIncoming: UInt64 = 0
    private var replayMask: UInt64 = 0
    private var receivedAny = false
    private var bound = false
    private var running = false

    init(onFrame: @escaping FrameHandler, onState: @escaping StateHandler) {
        frameHandler = onFrame
        stateHandler = onState
    }

    func connect(_ grant: AppleVoiceGrant) async throws {
        close()
        guard !grant.p2pEnabled, grant.port > 0,
              let expected = Data(hex: grant.certificateFingerprint), expected.count == 32 else {
            throw AppleVoiceTransportError.invalidGrant
        }
        let session = try await join(grant, expected: expected)
        guard let uuid = UUID(uuidString: session.sessionId),
              let keyData = Data(base64Encoded: session.udpKey), keyData.count == 32,
              let prefix = Data(base64Encoded: session.noncePrefix), prefix.count == 4 else {
            throw AppleVoiceTransportError.invalidSession
        }
        var value = uuid.uuid
        let uuidData = withUnsafeBytes(of: &value) { Data($0) }
        let endpoint = NWEndpoint.Host(grant.host)
        guard let port = NWEndpoint.Port(rawValue: grant.port) else {
            throw AppleVoiceTransportError.invalidGrant
        }
        let udp = NWConnection(host: endpoint, port: port, using: .udp)
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            var resumed = false
            udp.stateUpdateHandler = { state in
                self.queue.async {
                    guard !resumed else { return }
                    switch state {
                    case .ready:
                        resumed = true
                        continuation.resume()
                    case .failed(let error):
                        resumed = true
                        continuation.resume(throwing: error)
                    case .cancelled:
                        resumed = true
                        continuation.resume(throwing: AppleVoiceTransportError.udpUnavailable)
                    default: break
                    }
                }
            }
            queue.async {
                self.sessionId = uuidData
                self.key = keyData
                self.noncePrefix = prefix
                self.connection = udp
                self.outgoing = 0
                self.highestIncoming = 0
                self.replayMask = 0
                self.receivedAny = false
                self.bound = false
                self.running = true
                udp.start(queue: self.queue)
                self.receiveNext()
            }
        }
        for _ in 0..<10 {
            queue.async { self.sendSecure(Data([0x01]), flags: 0x01) }
            try await Task.sleep(nanoseconds: 300_000_000)
            if queue.sync(execute: { bound }) { stateHandler("connected"); return }
        }
        close()
        throw AppleVoiceTransportError.bindTimeout
    }

    func sendAudio(sequence: UInt16, timestamp: UInt32, flags: UInt8, opus: Data) {
        guard !opus.isEmpty, opus.count <= 1_275 else { return }
        queue.async {
            var payload = Data([0x02])
            payload.appendBigEndian(sequence)
            payload.appendBigEndian(timestamp)
            payload.append(flags)
            payload.append(opus)
            self.sendSecure(payload, flags: 0x02)
        }
    }

    func close() {
        queue.sync {
            running = false
            connection?.stateUpdateHandler = nil
            connection?.cancel()
            connection = nil
            sessionId.resetBytes(in: 0..<sessionId.count)
            key.resetBytes(in: 0..<key.count)
            noncePrefix.resetBytes(in: 0..<noncePrefix.count)
            sessionId.removeAll(keepingCapacity: false)
            key.removeAll(keepingCapacity: false)
            noncePrefix.removeAll(keepingCapacity: false)
            bound = false
        }
        stateHandler("disconnected")
    }

    private struct JoinSession: Decodable { let sessionId: String; let udpKey: String; let noncePrefix: String }

    private func join(_ grant: AppleVoiceGrant, expected: Data) async throws -> JoinSession {
        guard let url = URL(string: "https://\(grant.host):\(grant.port)/join") else {
            throw AppleVoiceTransportError.invalidGrant
        }
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 8
        configuration.timeoutIntervalForResource = 10
        let delegate = VoicePinDelegate(expected: expected)
        let session = URLSession(configuration: configuration, delegate: delegate,
                                 delegateQueue: OperationQueue())
        defer { session.finishTasksAndInvalidate() }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json; charset=utf-8", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: ["grant": grant.grant])
        do {
            let (data, response) = try await session.data(for: request)
            guard data.count <= 64 * 1_024, let http = response as? HTTPURLResponse,
                  http.statusCode == 200 else { throw AppleVoiceTransportError.joinRejected }
            return try JSONDecoder().decode(JoinSession.self, from: data)
        } catch let error as URLError where error.code == .serverCertificateUntrusted
                                           || error.code == .secureConnectionFailed {
            throw AppleVoiceTransportError.certificateMismatch
        }
    }

    private func sendSecure(_ plaintext: Data, flags: UInt8) {
        guard running, let connection, plaintext.count <= 1_400 else { return }
        outgoing &+= 1
        var header = Data([0x53, 0x53, 0x03, flags])
        header.append(sessionId)
        header.appendBigEndian(outgoing)
        var nonceData = noncePrefix
        nonceData.appendBigEndian(outgoing)
        do {
            let nonce = try ChaChaPoly.Nonce(data: nonceData)
            let sealed = try ChaChaPoly.seal(plaintext, using: SymmetricKey(data: key),
                                             nonce: nonce, authenticating: header)
            var packet = header
            packet.append(sealed.ciphertext)
            packet.append(sealed.tag)
            connection.send(content: packet, completion: .contentProcessed { _ in })
        } catch { stateHandler("encryption_failed") }
    }

    private func receiveNext() {
        guard running, let connection else { return }
        connection.receiveMessage { data, _, _, error in
            self.queue.async {
                if let data { self.receive(data) }
                if error == nil && self.running { self.receiveNext() }
                else if self.running { self.stateHandler("offline") }
            }
        }
    }

    private func receive(_ packet: Data) {
        guard packet.count >= 45, packet[0] == 0x53, packet[1] == 0x53, packet[2] == 3,
              packet.subdata(in: 4..<20) == sessionId else { return }
        let sequence: UInt64 = packet.bigEndian(at: 20)
        if replayed(sequence) { return }
        let header = packet.prefix(28)
        let cipherRange = 28..<(packet.count - 16)
        var nonceData = noncePrefix
        nonceData.appendBigEndian(sequence)
        do {
            let nonce = try ChaChaPoly.Nonce(data: nonceData)
            let box = try ChaChaPoly.SealedBox(nonce: nonce, ciphertext: packet[cipherRange],
                                               tag: packet.suffix(16))
            let clear = try ChaChaPoly.open(box, using: SymmetricKey(data: key), authenticating: header)
            accept(sequence)
            if clear.count == 1, clear[0] == 0x11 { bound = true; return }
            if clear.count >= 2, clear[0] == 0x13 { stateHandler("sleeping"); return }
            guard clear.count >= 10, clear[0] == 0x12 else { return }
            let senderBytes = Int(clear[1])
            let metadata = 2 + senderBytes
            guard senderBytes > 0, senderBytes <= 64, metadata + 7 < clear.count,
                  let sender = String(data: clear[2..<metadata], encoding: .utf8) else { return }
            let voice = AppleVoiceFrame(senderId: sender,
                sequence: clear.bigEndian(at: metadata),
                timestamp: clear.bigEndian(at: metadata + 2),
                flags: clear[metadata + 6], opus: clear.suffix(from: metadata + 7))
            frameHandler(voice)
        } catch { return }
    }

    private func replayed(_ sequence: UInt64) -> Bool {
        if !receivedAny || sequence > highestIncoming { return false }
        let delta = highestIncoming - sequence
        return delta >= 64 || (replayMask & (UInt64(1) << delta)) != 0
    }

    private func accept(_ sequence: UInt64) {
        if !receivedAny { receivedAny = true; highestIncoming = sequence; replayMask = 1; return }
        if sequence > highestIncoming {
            let shift = sequence - highestIncoming
            replayMask = shift >= 64 ? 1 : (replayMask << shift) | 1
            highestIncoming = sequence
        } else { replayMask |= UInt64(1) << (highestIncoming - sequence) }
    }
}

private extension Data {
    init?(hex: String) {
        guard hex.count.isMultiple(of: 2) else { return nil }
        self.init(capacity: hex.count / 2)
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            guard let value = UInt8(hex[index..<next], radix: 16) else { return nil }
            append(value)
            index = next
        }
    }

    mutating func appendBigEndian<T: FixedWidthInteger>(_ value: T) {
        var big = value.bigEndian
        Swift.withUnsafeBytes(of: &big) { append(contentsOf: $0) }
    }

    func bigEndian<T: FixedWidthInteger>(at offset: Int) -> T {
        let count = MemoryLayout<T>.size
        return subdata(in: offset..<(offset + count)).withUnsafeBytes {
            T(bigEndian: $0.loadUnaligned(as: T.self))
        }
    }
}
