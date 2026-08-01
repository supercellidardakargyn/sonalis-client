import AVFoundation
import Foundation

final class SonalisAudioEngine {
    // The buffer is valid only for the duration of this callback. Consumers
    // encode or copy into their fixed media ring synchronously.
    typealias Capture = (UnsafeBufferPointer<Float>, AVAudioTime) -> Void

    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private var capture: Capture?
    private var converter: AVAudioConverter?
    private var conversionBuffer: AVAudioPCMBuffer?
    private let targetFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                             sampleRate: 48_000, channels: 1,
                                             interleaved: false)!

    func start(listenOnly: Bool = false, capture: @escaping Capture) throws {
        stop()
#if os(iOS)
        let audioSession = AVAudioSession.sharedInstance()
        try audioSession.setCategory(listenOnly ? .playback : .playAndRecord,
                                     mode: listenOnly ? .default : .voiceChat,
                                     options: listenOnly ? [] : [.allowBluetooth, .defaultToSpeaker])
        try audioSession.setPreferredSampleRate(48_000)
        try audioSession.setPreferredIOBufferDuration(0.02)
        try audioSession.setActive(true)
#endif
        self.capture = capture
        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: targetFormat)
        if !listenOnly {
            let input = engine.inputNode
            let inputFormat = input.outputFormat(forBus: 0)
            guard inputFormat.sampleRate > 0, inputFormat.channelCount > 0 else {
                throw NSError(domain: "SonalisAudio", code: 1,
                              userInfo: [NSLocalizedDescriptionKey: "microphone_unavailable"])
            }
            converter = AVAudioConverter(from: inputFormat, to: targetFormat)
            conversionBuffer = AVAudioPCMBuffer(pcmFormat: targetFormat, frameCapacity: 8_192)
            input.installTap(onBus: 0, bufferSize: 960, format: inputFormat) { [weak self] buffer, time in
                guard let self, let converter = self.converter, let output = self.conversionBuffer else { return }
                output.frameLength = 0
                var supplied = false
                var conversionError: NSError?
                converter.convert(to: output, error: &conversionError) { _, status in
                    if supplied { status.pointee = .noDataNow; return nil }
                    supplied = true
                    status.pointee = .haveData
                    return buffer
                }
                guard conversionError == nil, let channel = output.floatChannelData?[0] else { return }
                self.capture?(UnsafeBufferPointer(start: channel, count: Int(output.frameLength)), time)
            }
        }
        engine.prepare()
        try engine.start()
        player.play()
    }

    func render(_ samples: [Float]) {
        guard engine.isRunning, !samples.isEmpty,
              let buffer = AVAudioPCMBuffer(pcmFormat: targetFormat,
                                            frameCapacity: AVAudioFrameCount(samples.count)) else { return }
        buffer.frameLength = AVAudioFrameCount(samples.count)
        samples.withUnsafeBufferPointer { input in
            if let output = buffer.floatChannelData?[0], let source = input.baseAddress {
                output.update(from: source, count: samples.count)
            }
        }
        player.scheduleBuffer(buffer, completionHandler: nil)
    }

    func stop() {
        if engine.inputNode.numberOfInputs > 0 { engine.inputNode.removeTap(onBus: 0) }
        player.stop()
        engine.stop()
        if player.engine != nil { engine.detach(player) }
        capture = nil
        converter = nil
        conversionBuffer = nil
#if os(iOS)
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
#endif
    }

    deinit { stop() }
}
