#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ss {

class NetworkClient;
class PeerStream;

struct AudioDeviceInfo {
    std::string id;
    std::string name;
};

struct AudioDiagnosticsSnapshot {
    std::uint64_t capturedPcmFrames{};
    std::uint64_t vadAcceptedFrames{};
    std::uint64_t vadRejectedFrames{};
    std::uint64_t opusEncodeSuccess{};
    std::uint64_t opusEncodeErrors{};
    std::uint64_t decodedOpusFrames{};
    std::uint64_t wasapiRenderFrames{};
    std::uint64_t fecRecoveries{};
    std::uint64_t plcFrames{};
    float jitterMs{};
    float packetLossPercent{};
    std::uint64_t lastCaptureAgeMs{};
    std::uint64_t lastReceiveAgeMs{};
};

class AudioEngine final {
public:
    static constexpr std::size_t kMaximumMixedPeers = 64;
    using ActivityCallback = std::function<void()>;
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    static std::vector<AudioDeviceInfo> EnumerateInputDevices();
    static std::vector<AudioDeviceInfo> EnumerateOutputDevices();

    bool Start(const std::string& inputDeviceId,
               const std::string& outputDeviceId,
               NetworkClient* network,
               bool voiceActivation,
               float vadSensitivity,
               std::string& error);
    void Stop();
    void SetVoiceActivation(bool enabled) noexcept;
    void SetPushToTalk(bool enabled) noexcept;
    void SetPushToTalkKey(int virtualKey) noexcept;
    void SetVadSensitivity(float sensitivity) noexcept;
    void SetLocalDenoise(bool enabled) noexcept;
    void SetEncoderBitrate(int bitrate) noexcept;
    void SetTransmitAllowed(bool allowed) noexcept;
    void SetMicrophoneMuted(bool muted) noexcept;
    void SetOutputMuted(bool muted) noexcept;
    void SetMasterOutputVolume(float volume) noexcept;
    void SetPeerVolume(std::uint32_t peerId, float volume);
    void SetPeerMuted(std::uint32_t peerId, bool muted);
    void ClearPeerMixControls() noexcept;
    void SetActivityCallback(ActivityCallback callback);
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] bool IsTransmitting() const noexcept;
    [[nodiscard]] bool HasCaptureDevice() const noexcept;
    [[nodiscard]] bool IsTransmitAllowed() const noexcept;
    [[nodiscard]] bool IsMicrophoneMuted() const noexcept;
    [[nodiscard]] bool IsOutputMuted() const noexcept;
    [[nodiscard]] float MasterOutputVolume() const noexcept;
    [[nodiscard]] float PeerVolume(std::uint32_t peerId) const noexcept;
    [[nodiscard]] bool IsPeerMuted(std::uint32_t peerId) const noexcept;
    [[nodiscard]] float MicrophoneLevel() const noexcept;
    [[nodiscard]] float MicrophoneRmsDbfs() const noexcept;
    [[nodiscard]] float VoiceThresholdLevel() const noexcept;
    [[nodiscard]] float PeerLevel(std::uint32_t peerId) const noexcept;
    [[nodiscard]] bool PeerSpeaking(std::uint32_t peerId) const noexcept;
    [[nodiscard]] bool HasActivePeerAudio() const noexcept;
    [[nodiscard]] std::string LastError() const;
    [[nodiscard]] AudioDiagnosticsSnapshot Diagnostics() const noexcept;

    void SubmitPacket(std::uint32_t peerId,
                      std::uint16_t sequence,
                      std::uint32_t timestamp,
                      std::uint8_t flags,
                      std::span<const std::uint8_t> opus);
    void RemovePeer(std::uint32_t peerId);

private:
    struct PeerMixControl {
        float volume{1.0F};
        bool muted{false};
    };

    void CaptureLoop();
    void RenderLoop();
    void RunRenderSession(bool startupProbe = false);
    void MixNextBlock(float* output) noexcept;
    void SetError(std::string error);
    void NotifyActivity();
    void NotifyMicrophoneMeter(float level);
    void NotifyPeerMeter();
    void CompleteStartup(bool capture, bool succeeded, std::string error = {});

    std::atomic<bool> running_{false};
    std::atomic<bool> voiceActivation_{true};
    std::atomic<bool> pushToTalk_{false};
    std::atomic<int> pushToTalkVirtualKey_{0x56};
    std::atomic<float> vadSensitivity_{0.62F};
    std::atomic<bool> localDenoise_{true};
    std::atomic<int> encoderBitrate_{24'000};
    std::atomic<bool> transmitAllowed_{true};
    std::atomic<bool> microphoneMuted_{false};
    std::atomic<bool> outputMuted_{false};
    std::atomic<float> masterOutputVolume_{1.0F};
    std::atomic<bool> transmitting_{false};
    std::atomic<bool> captureAvailable_{false};
    std::atomic<float> microphoneLevel_{0.0F};
    std::atomic<float> microphoneRmsDbfs_{-96.0F};
    std::atomic<float> voiceThresholdLevel_{0.15F};
    std::atomic<std::uint64_t> lastMicrophoneMeterNotifyMs_{0};
    std::atomic<std::uint64_t> lastPeerMeterNotifyMs_{0};
    std::atomic<int> lastMicrophoneMeterBucket_{-1};
    NetworkClient* network_{};
    std::string inputDeviceId_;
    std::string outputDeviceId_;
    std::thread captureThread_;
    std::thread renderThread_;
    std::atomic<std::uint64_t> lastIncomingAudioMs_{0};
    std::atomic<std::uint64_t> lastCaptureAudioMs_{0};
    std::atomic<std::uint64_t> renderGeneration_{0};
    std::atomic<std::uint64_t> capturedPcmFrames_{0};
    std::atomic<std::uint64_t> vadAcceptedFrames_{0};
    std::atomic<std::uint64_t> vadRejectedFrames_{0};
    std::atomic<std::uint64_t> opusEncodeSuccess_{0};
    std::atomic<std::uint64_t> opusEncodeErrors_{0};
    std::atomic<std::uint64_t> decodedOpusFrames_{0};
    std::atomic<std::uint64_t> wasapiRenderFrames_{0};
    // Owned exclusively by the WASAPI render thread. A block-to-block release
    // prevents hard clipping when several peers speak without adding locks or
    // allocations to the hot path.
    float mixLimiterGain_{1.0F};
    std::mutex startupMutex_;
    std::condition_variable startupCv_;
    bool captureStartupComplete_{false};
    bool captureStartupSucceeded_{false};
    bool renderStartupComplete_{false};
    bool renderStartupSucceeded_{false};
    std::mutex renderWakeMutex_;
    std::condition_variable renderWakeCv_;
    mutable std::mutex streamsMutex_;
    std::unordered_map<std::uint32_t, std::shared_ptr<PeerStream>> streams_;
    std::unordered_map<std::uint32_t, PeerMixControl> peerMixControls_;
    mutable std::mutex errorMutex_;
    std::string lastError_;
    mutable std::mutex activityCallbackMutex_;
    ActivityCallback activityCallback_;
};

}  // namespace ss
