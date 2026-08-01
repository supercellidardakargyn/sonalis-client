import AVFoundation
import Foundation

final class SonalisVoiceCall: @unchecked Sendable {
    private let media = DispatchQueue(label: "tr.sonalis.voice.media", qos: .userInteractive)
    private let captureLock = NSLock()
    private let receiveLock = NSLock()
    private let audio = SonalisAudioEngine()
    private var transport: SonalisVoiceTransport?
    private var encoder: SonalisVoiceEncoder?
    private var decoders: [String: SonalisVoiceDecoder] = [:]
    private var captureFrames = Array(repeating: [Float](repeating: 0, count: 960), count: 8)
    private var captureWrite = 0
    private var captureRead = 0
    private var captureCount = 0
    private var partial = [Float](repeating: 0, count: 960)
    private var partialCount = 0
    private var received: [AppleVoiceFrame] = []
    private var timer: DispatchSourceTimer?
    private var sequence: UInt16 = 0
    private var timestamp: UInt32 = 0
    private var talking = false
    private var running = false
    private let state: @Sendable (String) -> Void

    init(onState: @escaping @Sendable (String) -> Void) { state = onState }

    func connect(_ grant: AppleVoiceGrant) async throws {
        close()
        let newEncoder = try SonalisVoiceEncoder(bitrate: grant.bitrate)
        let newTransport = SonalisVoiceTransport(onFrame: { [weak self] frame in
            guard let self else { return }
            self.receiveLock.lock()
            if self.received.count < 128 { self.received.append(frame) }
            self.receiveLock.unlock()
        }, onState: state)
        try await newTransport.connect(grant)
        encoder = newEncoder
        transport = newTransport
        running = true
        let clock = DispatchSource.makeTimerSource(queue: media)
        clock.schedule(deadline: .now(), repeating: .milliseconds(20), leeway: .milliseconds(2))
        clock.setEventHandler { [weak self] in self?.tick() }
        timer = clock
        clock.resume()
        do {
            try audio.start(listenOnly: !grant.canSpeak) { [weak self] samples, _ in
                self?.capture(samples)
            }
        } catch {
            close()
            throw error
        }
    }

    private func capture(_ samples: UnsafeBufferPointer<Float>) {
        captureLock.lock()
        defer { captureLock.unlock() }
        var offset = 0
        while offset < samples.count {
            let copied = min(samples.count - offset, 960 - partialCount)
            partial.withUnsafeMutableBufferPointer { target in
                guard let source = samples.baseAddress, let destination = target.baseAddress else { return }
                destination.advanced(by: partialCount).update(from: source.advanced(by: offset), count: copied)
            }
            partialCount += copied
            offset += copied
            if partialCount == 960 {
                if captureCount < captureFrames.count {
                    captureFrames[captureWrite].withUnsafeMutableBufferPointer { target in
                        partial.withUnsafeBufferPointer { source in
                            target.baseAddress?.update(from: source.baseAddress!, count: 960)
                        }
                    }
                    captureWrite = (captureWrite + 1) % captureFrames.count
                    captureCount += 1
                }
                partialCount = 0
            }
        }
    }

    private func tick() {
        guard running else { return }
        captureLock.lock()
        let hasCapture = captureCount > 0
        let captureIndex = captureRead
        if hasCapture { captureRead = (captureRead + 1) % captureFrames.count; captureCount -= 1 }
        captureLock.unlock()
        if hasCapture, let encoder, let transport {
            captureFrames[captureIndex].withUnsafeBufferPointer { samples in
                let rms = sonalis_voice_rms(samples.baseAddress, UInt32(samples.count))
                if rms >= 0.008 {
                    do {
                        let packet = try encoder.encode(samples)
                        sequence &+= 1
                        let flags: UInt8 = talking ? 0 : 0x01
                        talking = true
                        transport.sendAudio(sequence: sequence, timestamp: timestamp, flags: flags, opus: packet)
                    } catch { state("encode_failed") }
                } else { talking = false }
                timestamp &+= 960
            }
        }

        receiveLock.lock()
        let frames = received
        received.removeAll(keepingCapacity: true)
        receiveLock.unlock()
        guard !frames.isEmpty else { return }
        var mix = [Float](repeating: 0, count: 960)
        var talkers = 0
        for frame in frames {
            do {
                let decoder = try decoders[frame.senderId] ?? SonalisVoiceDecoder()
                decoders[frame.senderId] = decoder
                let decoded = try decoder.decode(frame.opus)
                for index in 0..<960 { mix[index] += decoded[index] }
                talkers += 1
            } catch { continue }
        }
        if talkers > 0 {
            let gain = 1 / sqrt(Float(talkers))
            for index in 0..<960 { mix[index] = min(1, max(-1, mix[index] * gain)) }
            audio.render(mix)
        }
    }

    func close() {
        running = false
        timer?.cancel()
        timer = nil
        audio.stop()
        transport?.close()
        transport = nil
        encoder = nil
        decoders.removeAll(keepingCapacity: false)
        receiveLock.lock(); received.removeAll(keepingCapacity: false); receiveLock.unlock()
        captureLock.lock()
        captureWrite = 0; captureRead = 0; captureCount = 0; partialCount = 0
        for index in partial.indices { partial[index] = 0 }
        captureLock.unlock()
    }

    deinit { close() }
}
