import Foundation

enum SonalisVoiceCodecError: Error { case unavailable, invalidFrame, encodeFailed, decodeFailed }

final class SonalisVoiceEncoder {
    private let handle: OpaquePointer

    init(bitrate: UInt32) throws {
        guard let value = sonalis_voice_encoder_create(bitrate) else {
            throw SonalisVoiceCodecError.unavailable
        }
        handle = value
    }

    func setBitrate(_ bitrate: UInt32) -> Bool {
        sonalis_voice_encoder_set_bitrate(handle, bitrate) == 1
    }

    func encode(_ samples: UnsafeBufferPointer<Float>) throws -> Data {
        guard samples.count == 960, let base = samples.baseAddress else {
            throw SonalisVoiceCodecError.invalidFrame
        }
        var output = Data(count: 1_275)
        let count = output.withUnsafeMutableBytes { bytes in
            sonalis_voice_encoder_encode(handle, base,
                bytes.bindMemory(to: UInt8.self).baseAddress, UInt32(bytes.count))
        }
        guard count > 0 else { throw SonalisVoiceCodecError.encodeFailed }
        output.removeSubrange(Int(count)..<output.count)
        return output
    }

    deinit { sonalis_voice_encoder_destroy(handle) }
}

final class SonalisVoiceDecoder {
    private let handle: OpaquePointer

    init() throws {
        guard let value = sonalis_voice_decoder_create() else {
            throw SonalisVoiceCodecError.unavailable
        }
        handle = value
    }

    func decode(_ packet: Data, fec: Bool = false) throws -> [Float] {
        guard !packet.isEmpty, packet.count <= 1_275 else {
            throw SonalisVoiceCodecError.invalidFrame
        }
        var output = [Float](repeating: 0, count: 960)
        let count = packet.withUnsafeBytes { packetBytes in
            output.withUnsafeMutableBufferPointer { samples in
                sonalis_voice_decoder_decode(handle,
                    packetBytes.bindMemory(to: UInt8.self).baseAddress, UInt32(packetBytes.count),
                    samples.baseAddress, fec ? 1 : 0)
            }
        }
        guard count == 960 else { throw SonalisVoiceCodecError.decodeFailed }
        return output
    }

    func conceal() throws -> [Float] {
        var output = [Float](repeating: 0, count: 960)
        let count = output.withUnsafeMutableBufferPointer {
            sonalis_voice_decoder_conceal(handle, $0.baseAddress)
        }
        guard count == 960 else { throw SonalisVoiceCodecError.decodeFailed }
        return output
    }

    func reset() { sonalis_voice_decoder_reset(handle) }
    deinit { sonalis_voice_decoder_destroy(handle) }
}
