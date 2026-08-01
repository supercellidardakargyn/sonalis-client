#include "audio_engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

#include "network.h"
#include "noise_suppressor.h"
#include "performance.h"
#include "opus_codec.h"
#include "protocol.h"
#include "settings.h"

namespace ss {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

std::uint64_t SteadyMilliseconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count());
}

std::string HResultText(const char* action, const HRESULT result) {
    std::ostringstream stream;
    stream << action << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << ')';
    return stream.str();
}

WAVEFORMATEX VoiceFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 1;
    format.nSamplesPerSec = kAudioSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

ComPtr<IMMDevice> ResolveDevice(const EDataFlow flow, const std::string& deviceId, HRESULT& result) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) return {};
    ComPtr<IMMDevice> device;
    if (deviceId.empty()) {
        result = enumerator->GetDefaultAudioEndpoint(flow, eCommunications, &device);
    } else {
        const std::wstring wideId = Utf8ToWide(deviceId);
        result = wideId.empty() ? E_INVALIDARG : enumerator->GetDevice(wideId.c_str(), &device);
    }
    return device;
}

bool InitializeSharedClient(const ComPtr<IMMDevice>& device,
                            ComPtr<IAudioClient3>& client,
                            HANDLE eventHandle,
                            UINT32& bufferFrames,
                            std::string& error) {
    HRESULT result = device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &client);
    if (FAILED(result)) {
        error = HResultText("IAudioClient3 etkinlestirilemedi", result);
        return false;
    }

    WAVEFORMATEX format = VoiceFormat();
    UINT32 defaultPeriod = 0;
    UINT32 fundamentalPeriod = 0;
    UINT32 minimumPeriod = 0;
    UINT32 maximumPeriod = 0;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
        | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
        | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    result = client->GetSharedModeEnginePeriod(&format, &defaultPeriod, &fundamentalPeriod, &minimumPeriod, &maximumPeriod);
    if (SUCCEEDED(result)) {
        // The minimum engine period can wake the CPU every 2-3 ms. Voice packets
        // are 20 ms, so the normal shared-mode period keeps latency while greatly
        // reducing idle wakeups and context switches.
        result = client->InitializeSharedAudioStream(flags, defaultPeriod, &format, nullptr);
    }
    if (FAILED(result)) {
        constexpr REFERENCE_TIME kTwentyMilliseconds = 200'000;
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kTwentyMilliseconds, 0, &format, nullptr);
    }
    if (FAILED(result)) {
        error = HResultText("48 kHz mono WASAPI akisi acilamadi", result);
        return false;
    }
    result = client->SetEventHandle(eventHandle);
    if (FAILED(result)) {
        error = HResultText("WASAPI olay taniticisi ayarlanamadi", result);
        return false;
    }
    result = client->GetBufferSize(&bufferFrames);
    if (FAILED(result)) {
        error = HResultText("WASAPI tampon boyutu alinamadi", result);
        return false;
    }
    return true;
}

std::vector<AudioDeviceInfo> Enumerate(const EDataFlow flow) {
    std::vector<AudioDeviceInfo> devices;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        if (uninitialize) CoUninitialize();
        return devices;
    }
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) {
        if (uninitialize) CoUninitialize();
        return devices;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, &device))) continue;
        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId)) || rawId == nullptr) continue;
        std::wstring name(rawId);
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
                name = value.pwszVal;
            }
            PropVariantClear(&value);
        }
        devices.push_back({WideToUtf8(rawId), WideToUtf8(name)});
        CoTaskMemFree(rawId);
    }
    std::sort(devices.begin(), devices.end(), [](const AudioDeviceInfo& left, const AudioDeviceInfo& right) {
        return left.name < right.name;
    });
    if (uninitialize) CoUninitialize();
    return devices;
}

int SequenceDistance(const std::uint16_t newer, const std::uint16_t older) {
    return static_cast<std::int16_t>(newer - older);
}

}  // namespace

class PeerStream final {
public:
    void Push(const std::uint16_t sequence,
              const std::uint32_t timestamp,
              const std::uint8_t flags,
              const std::span<const std::uint8_t> packet) {
        if (packet.empty() || packet.size() > protocol::kMaxOpusBytes) return;
        std::scoped_lock lock(mutex_);
        if ((flags & protocol::kAudioFlagTalkStart) != 0) {
            for (auto& item : slots_) item.occupied = false;
            decoder_.Reset();
            receivedAny_ = false;
            started_ = false;
        }
        const auto arrival = Clock::now();
        const int sequenceAdvance = receivedAny_ ? SequenceDistance(sequence, newestSequence_) : 0;
        if (receivedAny_ && sequenceAdvance > 0) {
            const float arrivalDeltaMs = static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(
                arrival - newestArrival_).count()) / 1000.0F;
            const std::uint32_t timestampDelta = timestamp - newestTimestamp_;
            const float mediaDeltaMs = static_cast<float>(timestampDelta) * 1000.0F
                / static_cast<float>(kAudioSampleRate);
            if (mediaDeltaMs > 0.0F && mediaDeltaMs < 1'000.0F) {
                const float variation = std::abs(arrivalDeltaMs - mediaDeltaMs);
                jitterMs_ += (variation - jitterMs_) / 16.0F;
            }
            const float observedLoss = sequenceAdvance > 1
                ? static_cast<float>(sequenceAdvance - 1) / static_cast<float>(sequenceAdvance) : 0.0F;
            packetLossEma_ += (observedLoss - packetLossEma_) * 0.08F;
            const int jitterPackets = static_cast<int>(std::ceil(jitterMs_ / 20.0F));
            const int lossPackets = packetLossEma_ >= 0.10F ? 2 : (packetLossEma_ >= 0.03F ? 1 : 0);
            targetPackets_ = static_cast<std::size_t>(std::clamp(2 + jitterPackets + lossPackets, 2, 6));
        }
        Slot& slot = slots_[sequence % slots_.size()];
        slot.occupied = true;
        slot.sequence = sequence;
        slot.timestamp = timestamp;
        slot.flags = flags;
        slot.size = static_cast<std::uint16_t>(packet.size());
        std::copy(packet.begin(), packet.end(), slot.data.begin());
        if (!receivedAny_) {
            firstSequence_ = sequence;
            newestSequence_ = sequence;
            receivedAny_ = true;
        } else if (SequenceDistance(sequence, newestSequence_) > 0) {
            newestSequence_ = sequence;
            newestTimestamp_ = timestamp;
            newestArrival_ = arrival;
        }
        if (newestSequence_ == sequence) {
            newestTimestamp_ = timestamp;
            newestArrival_ = arrival;
        }
        lastPacket_ = arrival;
    }

    bool Decode(float* output) noexcept {
        if (output == nullptr || !decoder_.IsValid()) return false;
        std::scoped_lock lock(mutex_);
        const auto now = Clock::now();
        if (!receivedAny_ || now - lastPacket_ > std::chrono::seconds(2)) {
            level_.store(level_.load() * 0.7F);
            return false;
        }

        if (!started_) {
            std::size_t available = 0;
            for (const auto& slot : slots_) available += slot.occupied ? 1U : 0U;
            if (available < targetPackets_) return false;
            expectedSequence_ = firstSequence_;
            started_ = true;
        }
        if (SequenceDistance(newestSequence_, expectedSequence_) > 16) {
            expectedSequence_ = static_cast<std::uint16_t>(newestSequence_ - 1);
        }

        Slot& expected = slots_[expectedSequence_ % slots_.size()];
        int decoded = 0;
        if (expected.occupied && expected.sequence == expectedSequence_) {
            decoded = decoder_.Decode(expected.data.data(), expected.size, output, false);
            expected.occupied = false;
        } else {
            const std::uint16_t followingSequence = static_cast<std::uint16_t>(expectedSequence_ + 1);
            Slot& following = slots_[followingSequence % slots_.size()];
            if (following.occupied && following.sequence == followingSequence) {
                decoded = decoder_.Decode(following.data.data(), following.size, output, true);
                if (decoded > 0) ++fecRecoveries_;
            } else {
                decoded = decoder_.Decode(nullptr, 0, output, false);
                if (decoded > 0) ++plcFrames_;
            }
        }
        expectedSequence_ = static_cast<std::uint16_t>(expectedSequence_ + 1);
        if (decoded <= 0) return false;
        if (decoded < kOpusFrameSamples) {
            std::fill(output + decoded, output + kOpusFrameSamples, 0.0F);
        }
        const float target = RmsToMeter(ComputeRms(output, kOpusFrameSamples));
        level_.store(SmoothMeterLevel(level_.load(std::memory_order_relaxed), target),
                     std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] float Level() const noexcept { return level_.load(); }
    [[nodiscard]] bool IsRecentlyActive() const noexcept {
        std::scoped_lock lock(mutex_);
        return receivedAny_ && Clock::now() - lastPacket_ < std::chrono::milliseconds(280);
    }
    [[nodiscard]] std::uint64_t FecRecoveries() const noexcept { std::scoped_lock lock(mutex_); return fecRecoveries_; }
    [[nodiscard]] std::uint64_t PlcFrames() const noexcept { std::scoped_lock lock(mutex_); return plcFrames_; }
    [[nodiscard]] float JitterMs() const noexcept { std::scoped_lock lock(mutex_); return jitterMs_; }
    [[nodiscard]] float PacketLossPercent() const noexcept {
        std::scoped_lock lock(mutex_);
        return packetLossEma_ * 100.0F;
    }

private:
    struct Slot {
        bool occupied{false};
        std::uint16_t sequence{};
        std::uint32_t timestamp{};
        std::uint8_t flags{};
        std::uint16_t size{};
        std::array<std::uint8_t, protocol::kMaxOpusBytes> data{};
    };

    VoiceDecoder decoder_;
    mutable std::mutex mutex_;
    std::array<Slot, 32> slots_{};
    bool receivedAny_{false};
    bool started_{false};
    std::uint16_t firstSequence_{};
    std::uint16_t newestSequence_{};
    std::uint16_t expectedSequence_{};
    std::uint32_t newestTimestamp_{};
    Clock::time_point newestArrival_{Clock::now()};
    float jitterMs_{};
    float packetLossEma_{};
    std::size_t targetPackets_{2};
    std::uint64_t fecRecoveries_{};
    std::uint64_t plcFrames_{};
    Clock::time_point lastPacket_{Clock::now()};
    std::atomic<float> level_{0.0F};
};

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { Stop(); }

std::vector<AudioDeviceInfo> AudioEngine::EnumerateInputDevices() { return Enumerate(eCapture); }
std::vector<AudioDeviceInfo> AudioEngine::EnumerateOutputDevices() { return Enumerate(eRender); }

bool AudioEngine::Start(const std::string& inputDeviceId,
                        const std::string& outputDeviceId,
                        NetworkClient* network,
                        const bool voiceActivation,
                        const float vadSensitivity,
                        std::string& error) {
    Stop();
    if (network == nullptr) {
        error = "Ag istemcisi gerekli";
        return false;
    }
    inputDeviceId_ = inputDeviceId;
    outputDeviceId_ = outputDeviceId;
    network_ = network;
    voiceActivation_.store(voiceActivation);
    vadSensitivity_.store(std::clamp(vadSensitivity, 0.0F, 1.0F));
    transmitting_.store(false);
    captureAvailable_.store(false);
    microphoneLevel_.store(0.0F);
    microphoneRmsDbfs_.store(-96.0F);
    lastMicrophoneMeterNotifyMs_.store(0);
    lastPeerMeterNotifyMs_.store(0);
    lastMicrophoneMeterBucket_.store(-1);
    lastIncomingAudioMs_.store(0);
    lastCaptureAudioMs_.store(0);
    renderGeneration_.store(0);
    capturedPcmFrames_.store(0);
    vadAcceptedFrames_.store(0);
    vadRejectedFrames_.store(0);
    opusEncodeSuccess_.store(0);
    opusEncodeErrors_.store(0);
    decodedOpusFrames_.store(0);
    wasapiRenderFrames_.store(0);
    mixLimiterGain_ = 1.0F;
    {
        std::scoped_lock lock(startupMutex_);
        captureStartupComplete_ = false;
        captureStartupSucceeded_ = false;
        renderStartupComplete_ = false;
        renderStartupSucceeded_ = false;
    }
    {
        std::scoped_lock lock(errorMutex_);
        lastError_.clear();
    }
    running_.store(true);
    captureThread_ = std::thread(&AudioEngine::CaptureLoop, this);
    renderThread_ = std::thread(&AudioEngine::RenderLoop, this);
    bool started = false;
    {
        std::unique_lock lock(startupMutex_);
        started = startupCv_.wait_for(lock, std::chrono::seconds(4), [this] {
            return captureStartupComplete_ && renderStartupComplete_;
        }) && renderStartupSucceeded_;
    }
    if (!started) {
        if (LastError().empty()) SetError("Ses cihazlari dort saniye icinde baslatilamadi");
        error = LastError();
        Stop();
        return false;
    }
    return true;
}

void AudioEngine::Stop() {
    running_.store(false);
    transmitting_.store(false);
    renderWakeCv_.notify_all();
    if (captureThread_.joinable()) captureThread_.join();
    if (renderThread_.joinable()) renderThread_.join();
    {
        std::scoped_lock lock(streamsMutex_);
        streams_.clear();
    }
    network_ = nullptr;
    captureAvailable_.store(false);
    microphoneLevel_.store(0.0F);
    microphoneRmsDbfs_.store(-96.0F);
}

void AudioEngine::SetVoiceActivation(const bool enabled) noexcept { voiceActivation_.store(enabled); }
void AudioEngine::SetPushToTalk(const bool enabled) noexcept { pushToTalk_.store(enabled); }
void AudioEngine::SetPushToTalkKey(const int virtualKey) noexcept {
    pushToTalkVirtualKey_.store(std::clamp(virtualKey, 1, 255));
}
void AudioEngine::SetVadSensitivity(const float sensitivity) noexcept {
    vadSensitivity_.store(std::clamp(sensitivity, 0.0F, 1.0F));
}
void AudioEngine::SetLocalDenoise(const bool enabled) noexcept { localDenoise_.store(enabled); }
void AudioEngine::SetEncoderBitrate(const int bitrate) noexcept {
    encoderBitrate_.store(std::clamp(bitrate, 12'000, 64'000));
}
void AudioEngine::SetTransmitAllowed(const bool allowed) noexcept {
    transmitAllowed_.store(allowed, std::memory_order_release);
    if (!allowed) transmitting_.store(false, std::memory_order_release);
}
void AudioEngine::SetMicrophoneMuted(const bool muted) noexcept {
    microphoneMuted_.store(muted);
    if (muted) {
        transmitting_.store(false);
        microphoneLevel_.store(0.0F);
    }
}
void AudioEngine::SetOutputMuted(const bool muted) noexcept {
    outputMuted_.store(muted);
    if (!muted) return;
    {
        std::scoped_lock lock(streamsMutex_);
        streams_.clear();
    }
    lastIncomingAudioMs_.store(0);
}
void AudioEngine::SetMasterOutputVolume(const float volume) noexcept {
    const float clamped = std::clamp(volume, 0.0F, 2.0F);
    masterOutputVolume_.store(clamped);
    if (clamped > 0.0F) return;
    {
        std::scoped_lock lock(streamsMutex_);
        streams_.clear();
    }
    lastIncomingAudioMs_.store(0);
}
void AudioEngine::SetPeerVolume(const std::uint32_t peerId, const float volume) {
    if (peerId == 0) return;
    std::scoped_lock lock(streamsMutex_);
    const float clamped = std::clamp(volume, 0.0F, 2.0F);
    peerMixControls_[peerId].volume = clamped;
    if (clamped <= 0.0F) streams_.erase(peerId);
    if (streams_.empty()) lastIncomingAudioMs_.store(0);
}
void AudioEngine::SetPeerMuted(const std::uint32_t peerId, const bool muted) {
    if (peerId == 0) return;
    std::scoped_lock lock(streamsMutex_);
    peerMixControls_[peerId].muted = muted;
    if (muted) streams_.erase(peerId);
    if (streams_.empty()) lastIncomingAudioMs_.store(0);
}
void AudioEngine::ClearPeerMixControls() noexcept {
    std::scoped_lock lock(streamsMutex_);
    peerMixControls_.clear();
}
void AudioEngine::SetActivityCallback(ActivityCallback callback) {
    std::scoped_lock lock(activityCallbackMutex_);
    activityCallback_ = std::move(callback);
}
bool AudioEngine::IsRunning() const noexcept { return running_.load(); }
bool AudioEngine::IsTransmitting() const noexcept { return transmitting_.load(); }
bool AudioEngine::HasCaptureDevice() const noexcept { return captureAvailable_.load(); }
bool AudioEngine::IsTransmitAllowed() const noexcept { return transmitAllowed_.load(std::memory_order_acquire); }
bool AudioEngine::IsMicrophoneMuted() const noexcept { return microphoneMuted_.load(); }
bool AudioEngine::IsOutputMuted() const noexcept { return outputMuted_.load(); }
float AudioEngine::MasterOutputVolume() const noexcept { return masterOutputVolume_.load(); }
float AudioEngine::PeerVolume(const std::uint32_t peerId) const noexcept {
    std::scoped_lock lock(streamsMutex_);
    const auto found = peerMixControls_.find(peerId);
    return found == peerMixControls_.end() ? 1.0F : found->second.volume;
}
bool AudioEngine::IsPeerMuted(const std::uint32_t peerId) const noexcept {
    std::scoped_lock lock(streamsMutex_);
    const auto found = peerMixControls_.find(peerId);
    return found != peerMixControls_.end() && found->second.muted;
}
float AudioEngine::MicrophoneLevel() const noexcept { return microphoneLevel_.load(); }
float AudioEngine::MicrophoneRmsDbfs() const noexcept { return microphoneRmsDbfs_.load(); }
float AudioEngine::VoiceThresholdLevel() const noexcept { return voiceThresholdLevel_.load(); }

float AudioEngine::PeerLevel(const std::uint32_t peerId) const noexcept {
    std::shared_ptr<PeerStream> stream;
    float volume = 1.0F;
    {
        std::scoped_lock lock(streamsMutex_);
        const auto found = streams_.find(peerId);
        if (found == streams_.end()) return 0.0F;
        const auto control = peerMixControls_.find(peerId);
        if (outputMuted_.load() || (control != peerMixControls_.end() && control->second.muted)) return 0.0F;
        if (control != peerMixControls_.end()) volume = control->second.volume;
        if (volume <= 0.0F) return 0.0F;
        stream = found->second;
    }
    return std::clamp(stream->Level() * volume, 0.0F, 1.0F);
}

bool AudioEngine::PeerSpeaking(const std::uint32_t peerId) const noexcept {
    std::shared_ptr<PeerStream> stream;
    {
        std::scoped_lock lock(streamsMutex_);
        const auto found = streams_.find(peerId);
        if (found == streams_.end() || outputMuted_.load()) return false;
        const auto control = peerMixControls_.find(peerId);
        if (control != peerMixControls_.end()
            && (control->second.muted || control->second.volume <= 0.0F)) return false;
        stream = found->second;
    }
    return stream && stream->IsRecentlyActive();
}

bool AudioEngine::HasActivePeerAudio() const noexcept {
    if (outputMuted_.load() || masterOutputVolume_.load() <= 0.0F) return false;
    std::scoped_lock lock(streamsMutex_);
    for (const auto& [peerId, stream] : streams_) {
        const auto control = peerMixControls_.find(peerId);
        if (control != peerMixControls_.end() && (control->second.muted || control->second.volume <= 0.0F)) continue;
        const float volume = control == peerMixControls_.end() ? 1.0F : control->second.volume;
        if (stream && (stream->IsRecentlyActive() || stream->Level() * volume > 0.015F)) return true;
    }
    return false;
}

std::string AudioEngine::LastError() const {
    std::scoped_lock lock(errorMutex_);
    return lastError_;
}

AudioDiagnosticsSnapshot AudioEngine::Diagnostics() const noexcept {
    AudioDiagnosticsSnapshot snapshot{};
    const std::uint64_t now = SteadyMilliseconds();
    snapshot.capturedPcmFrames = capturedPcmFrames_.load(std::memory_order_relaxed);
    snapshot.vadAcceptedFrames = vadAcceptedFrames_.load(std::memory_order_relaxed);
    snapshot.vadRejectedFrames = vadRejectedFrames_.load(std::memory_order_relaxed);
    snapshot.opusEncodeSuccess = opusEncodeSuccess_.load(std::memory_order_relaxed);
    snapshot.opusEncodeErrors = opusEncodeErrors_.load(std::memory_order_relaxed);
    snapshot.decodedOpusFrames = decodedOpusFrames_.load(std::memory_order_relaxed);
    snapshot.wasapiRenderFrames = wasapiRenderFrames_.load(std::memory_order_relaxed);
    const std::uint64_t lastCapture = lastCaptureAudioMs_.load(std::memory_order_relaxed);
    const std::uint64_t lastReceive = lastIncomingAudioMs_.load(std::memory_order_relaxed);
    snapshot.lastCaptureAgeMs = lastCapture == 0 ? 0 : now - lastCapture;
    snapshot.lastReceiveAgeMs = lastReceive == 0 ? 0 : now - lastReceive;
    std::scoped_lock lock(streamsMutex_);
    for (const auto& [peerId, stream] : streams_) {
        (void)peerId;
        if (!stream) continue;
        snapshot.fecRecoveries += stream->FecRecoveries();
        snapshot.plcFrames += stream->PlcFrames();
        snapshot.jitterMs = std::max(snapshot.jitterMs, stream->JitterMs());
        snapshot.packetLossPercent = std::max(snapshot.packetLossPercent, stream->PacketLossPercent());
    }
    return snapshot;
}

void AudioEngine::SubmitPacket(const std::uint32_t peerId,
                               const std::uint16_t sequence,
                               const std::uint32_t timestamp,
                               const std::uint8_t flags,
                               const std::span<const std::uint8_t> opus) {
    if (!running_.load() || peerId == 0 || outputMuted_.load() || masterOutputVolume_.load() <= 0.0F) return;
    std::shared_ptr<PeerStream> stream;
    {
        std::scoped_lock lock(streamsMutex_);
        const auto control = peerMixControls_.find(peerId);
        if (control != peerMixControls_.end() && (control->second.muted || control->second.volume <= 0.0F)) return;
        auto& entry = streams_[peerId];
        if (!entry) entry = std::make_shared<PeerStream>();
        stream = entry;
    }
    stream->Push(sequence, timestamp, flags, opus);
    if (outputMuted_.load() || masterOutputVolume_.load() <= 0.0F) return;
    const std::uint64_t now = SteadyMilliseconds();
    const std::uint64_t previousIncoming = lastIncomingAudioMs_.exchange(now);
    if (now - previousIncoming > 200) NotifyActivity();
    NotifyPeerMeter();
    renderGeneration_.fetch_add(1);
    renderWakeCv_.notify_one();
}

void AudioEngine::RemovePeer(const std::uint32_t peerId) {
    std::scoped_lock lock(streamsMutex_);
    streams_.erase(peerId);
    peerMixControls_.erase(peerId);
}

void AudioEngine::SetError(std::string error) {
    std::scoped_lock lock(errorMutex_);
    if (lastError_.empty()) lastError_ = std::move(error);
}

void AudioEngine::CompleteStartup(const bool capture, const bool succeeded, std::string error) {
    if (capture) captureAvailable_.store(succeeded);
    // Missing capture is a non-fatal listen-only state. Do not let that warning
    // mask a later render/output failure in LastError().
    if (!error.empty() && (!capture || succeeded)) SetError(std::move(error));
    {
        std::scoped_lock lock(startupMutex_);
        bool& complete = capture ? captureStartupComplete_ : renderStartupComplete_;
        bool& success = capture ? captureStartupSucceeded_ : renderStartupSucceeded_;
        if (complete) return;
        complete = true;
        success = succeeded;
    }
    startupCv_.notify_all();
}

void AudioEngine::NotifyActivity() {
    ActivityCallback callback;
    { std::scoped_lock lock(activityCallbackMutex_); callback = activityCallback_; }
    if (callback) callback();
}

void AudioEngine::NotifyMicrophoneMeter(const float level) {
    constexpr std::uint64_t kMinimumUiIntervalMs = 67;
    const int bucket = std::clamp(static_cast<int>(std::lround(level * 128.0F)), 0, 128);
    if (bucket == lastMicrophoneMeterBucket_.load(std::memory_order_relaxed)) return;
    const std::uint64_t now = SteadyMilliseconds();
    std::uint64_t previous = lastMicrophoneMeterNotifyMs_.load(std::memory_order_relaxed);
    if (now - previous < kMinimumUiIntervalMs
        || !lastMicrophoneMeterNotifyMs_.compare_exchange_strong(
            previous, now, std::memory_order_relaxed)) return;
    lastMicrophoneMeterBucket_.store(bucket, std::memory_order_relaxed);
    NotifyActivity();
}

void AudioEngine::NotifyPeerMeter() {
    constexpr std::uint64_t kMinimumUiIntervalMs = 67;
    const std::uint64_t now = SteadyMilliseconds();
    std::uint64_t previous = lastPeerMeterNotifyMs_.load(std::memory_order_relaxed);
    if (now - previous < kMinimumUiIntervalMs) return;
    if (lastPeerMeterNotifyMs_.compare_exchange_strong(
            previous, now, std::memory_order_relaxed)) NotifyActivity();
}

void AudioEngine::CaptureLoop() {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult)) {
        CompleteStartup(true, false, HResultText("Capture COM baslatilamadi", comResult));
        return;
    }
    SetThreadDescription(GetCurrentThread(), L"Sonalis Capture");
    DWORD taskIndex = 0;
    // The shared 20 ms voice pipeline does not need the system-starving Pro
    // Audio profile. The Audio MMCSS class preserves deadlines while keeping
    // the desktop/UI responsive during Opus bursts.
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (eventHandle == nullptr) {
        CompleteStartup(true, false, "Capture olay taniticisi olusturulamadi");
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
        return;
    }

    HRESULT result = S_OK;
    ComPtr<IMMDevice> device = ResolveDevice(eCapture, inputDeviceId_, result);
    ComPtr<IAudioClient3> client;
    UINT32 bufferFrames = 0;
    std::string error;
    if (!device || FAILED(result) || !InitializeSharedClient(device, client, eventHandle, bufferFrames, error)) {
        CompleteStartup(true, false, device ? error : HResultText("Mikrofon acilamadi", result));
        CloseHandle(eventHandle);
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
        return;
    }
    ComPtr<IAudioCaptureClient> capture;
    result = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(result)) result = client->Start();
    if (FAILED(result)) {
        CompleteStartup(true, false, HResultText("Mikrofon akisi baslatilamadi", result));
        CloseHandle(eventHandle);
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
        return;
    }

    int configuredBitrate = encoderBitrate_.load(std::memory_order_relaxed);
    VoiceEncoder encoder(configuredBitrate);
    std::unique_ptr<NoiseSuppressor> noiseSuppressor;
    bool configuredDenoise = localDenoise_.load(std::memory_order_relaxed);
    if (configuredDenoise) noiseSuppressor = std::make_unique<NoiseSuppressor>();
    if (!encoder.IsValid()) {
        CompleteStartup(true, false, "Opus encoder olusturulamadi: " + encoder.Error());
        client->Stop();
        CloseHandle(eventHandle);
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
        return;
    }
    CompleteStartup(true, true);
    std::array<float, kOpusFrameSamples> opusFrame{};
    std::array<float, kOpusFrameSamples> previousFrame{};
    std::array<float, kOpusFrameSamples> previousFrame2{};
    std::array<std::uint8_t, kOpusPacketCapacity> encoded{};
    int opusOffset = 0;
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t previousTimestamp = 0;
    float noiseFloor = 0.0025F;
    int attackFrames = 0;
    int hangoverFrames = 0;
    bool gateOpen = false;
    bool havePrevious = false;
    bool havePrevious2 = false;

    const auto sendFrame = [&](const std::array<float, kOpusFrameSamples>& frame,
                               const std::uint32_t frameTimestamp,
                               const std::uint8_t packetFlags) {
        const int requestedBitrate = encoderBitrate_.load(std::memory_order_relaxed);
        if (requestedBitrate != configuredBitrate && encoder.SetBitrate(requestedBitrate)) {
            configuredBitrate = requestedBitrate;
        }
        const auto encodeStarted = Clock::now();
        const int bytes = encoder.Encode(frame.data(), encoded.data(), static_cast<int>(encoded.size()));
        RecordPerformance(PerformanceMetric::OpusEncode, static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - encodeStarted).count()));
        if (bytes <= 0) {
            opusEncodeErrors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        opusEncodeSuccess_.fetch_add(1, std::memory_order_relaxed);
        if (network_ != nullptr
            && network_->SendAudio(sequence, frameTimestamp,
                std::span<const std::uint8_t>(encoded.data(), static_cast<std::size_t>(bytes)), packetFlags)) {
            sequence = static_cast<std::uint16_t>(sequence + 1);
        }
    };

    while (running_.load()) {
        if (WaitForSingleObject(eventHandle, 100) != WAIT_OBJECT_0) continue;
        UINT32 packetFrames = 0;
        while (running_.load() && SUCCEEDED(capture->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE* raw = nullptr;
            DWORD flags = 0;
            UINT32 frames = 0;
            result = capture->GetBuffer(&raw, &frames, &flags, nullptr, nullptr);
            if (FAILED(result)) break;
            capturedPcmFrames_.fetch_add(frames, std::memory_order_relaxed);
            if (microphoneMuted_.load() || !transmitAllowed_.load(std::memory_order_acquire)) {
                timestamp += static_cast<std::uint32_t>(opusOffset) + frames;
                opusOffset = 0;
                attackFrames = 0;
                hangoverFrames = 0;
                gateOpen = false;
                havePrevious = false;
                havePrevious2 = false;
                transmitting_.store(false);
                microphoneLevel_.store(0.0F);
                capture->ReleaseBuffer(frames);
                continue;
            }
            const float* samples = reinterpret_cast<const float*>(raw);
            for (UINT32 index = 0; index < frames; ++index) {
                const float sample = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || samples == nullptr
                    ? 0.0F : std::clamp(samples[index], -1.0F, 1.0F);
                opusFrame[opusOffset++] = sample;
                if (opusOffset == kOpusFrameSamples) {
                    lastCaptureAudioMs_.store(SteadyMilliseconds(), std::memory_order_relaxed);
                    const bool requestedDenoise = localDenoise_.load(std::memory_order_relaxed);
                    if (requestedDenoise != configuredDenoise) {
                        configuredDenoise = requestedDenoise;
                        if (configuredDenoise) noiseSuppressor = std::make_unique<NoiseSuppressor>();
                        else noiseSuppressor.reset();
                    }
                    const bool pushToTalk = pushToTalk_.load();
                    const bool automatic = voiceActivation_.load() && !pushToTalk;
                    const float sensitivity = vadSensitivity_.load();
                    const float absoluteFloor = 0.0018F + (1.0F - sensitivity) * 0.012F;
                    const float ratio = 1.7F + (1.0F - sensitivity) * 4.8F;
                    const float rawRms = ComputeRms(opusFrame.data(), kOpusFrameSamples);
                    const float preThreshold = automatic ? std::max(absoluteFloor, noiseFloor * ratio) : 0.0F;
                    const bool denoiseWorthwhile = !automatic || gateOpen || hangoverFrames > 0 || rawRms >= preThreshold * 0.72F;
                    float rms = rawRms;
                    if (noiseSuppressor && noiseSuppressor->IsValid() && denoiseWorthwhile) {
                        const auto rnnoiseStarted = Clock::now();
                        noiseSuppressor->Process(opusFrame.data(), kOpusFrameSamples);
                        RecordPerformance(PerformanceMetric::RnNoise, static_cast<std::uint32_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - rnnoiseStarted).count()));
                        rms = ComputeRms(opusFrame.data(), kOpusFrameSamples);
                    }
                    // The visible input meter always represents the captured
                    // microphone RMS. RNNoise may alter the VAD decision level,
                    // but must not make the user's physical input meter jump.
                    const float target = RmsToMeter(rawRms);
                    const float level = SmoothMeterLevel(
                        microphoneLevel_.load(std::memory_order_relaxed), target);
                    microphoneLevel_.store(level, std::memory_order_relaxed);
                    microphoneRmsDbfs_.store(RmsToDbfs(rawRms), std::memory_order_relaxed);

                    const bool pushToTalkPressed = !pushToTalk
                        || (GetAsyncKeyState(pushToTalkVirtualKey_.load(std::memory_order_relaxed)) & 0x8000) != 0;
                    const float threshold = automatic ? std::max(absoluteFloor, noiseFloor * ratio) : 0.0F;
                    voiceThresholdLevel_.store(RmsToMeter(threshold));
                    const bool above = pushToTalk ? pushToTalkPressed : (!automatic || rms >= threshold);

                    if (automatic && !gateOpen && !above) {
                        noiseFloor = std::clamp(noiseFloor * 0.985F + rms * 0.015F, 0.0005F, 0.035F);
                    }
                    if (above) {
                        attackFrames = std::min(attackFrames + 1, 2);
                        hangoverFrames = 15;
                    } else {
                        attackFrames = 0;
                        if (hangoverFrames > 0) --hangoverFrames;
                    }

                    const bool wasGateOpen = gateOpen;
                    const bool shouldOpen = !automatic || attackFrames >= 2 || (gateOpen && hangoverFrames > 0);
                    if (automatic) {
                        if (shouldOpen) vadAcceptedFrames_.fetch_add(1, std::memory_order_relaxed);
                        else vadRejectedFrames_.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (shouldOpen && !gateOpen && encoder.IsValid()) {
                        gateOpen = true;
                        if (havePrevious) {
                            if (havePrevious2) {
                                sendFrame(previousFrame2, previousTimestamp - kOpusFrameSamples,
                                          protocol::kAudioFlagTalkStart);
                                sendFrame(previousFrame, previousTimestamp, 0);
                            } else {
                                sendFrame(previousFrame, previousTimestamp, protocol::kAudioFlagTalkStart);
                            }
                            sendFrame(opusFrame, timestamp, 0);
                        } else {
                            sendFrame(opusFrame, timestamp, protocol::kAudioFlagTalkStart);
                        }
                    } else if (shouldOpen && encoder.IsValid()) {
                        sendFrame(opusFrame, timestamp, 0);
                    } else if (!shouldOpen) {
                        gateOpen = false;
                    }
                    transmitting_.store(gateOpen);
                    if (gateOpen != wasGateOpen) NotifyActivity();
                    NotifyMicrophoneMeter(level);
                    if (!gateOpen) {
                        previousFrame2 = previousFrame;
                        havePrevious2 = havePrevious;
                        previousFrame = opusFrame;
                        previousTimestamp = timestamp;
                        havePrevious = true;
                    }
                    timestamp += kOpusFrameSamples;
                    opusOffset = 0;
                }
            }
            capture->ReleaseBuffer(frames);
        }
    }

    client->Stop();
    transmitting_.store(false);
    microphoneLevel_.store(0.0F);
    microphoneRmsDbfs_.store(-96.0F);
    CloseHandle(eventHandle);
    if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

void AudioEngine::MixNextBlock(float* output) noexcept {
    if (output == nullptr) return;
    std::fill(output, output + kOpusFrameSamples, 0.0F);
    if (outputMuted_.load() || masterOutputVolume_.load() <= 0.0F) return;
    struct ActiveStream {
        std::shared_ptr<PeerStream> stream;
        float volume{1.0F};
    };
    // Plus rooms currently admit 32 people, but keeping a fixed 64-entry
    // snapshot gives headroom without allocating on the WASAPI render thread
    // or silently dropping later speakers.
    std::array<ActiveStream, AudioEngine::kMaximumMixedPeers> activeStreams{};
    std::size_t streamCount = 0;
    {
        std::scoped_lock lock(streamsMutex_);
        for (const auto& [peerId, stream] : streams_) {
            if (streamCount == activeStreams.size()) break;
            const auto control = peerMixControls_.find(peerId);
            if (control != peerMixControls_.end() && (control->second.muted || control->second.volume <= 0.0F)) continue;
            activeStreams[streamCount++] = {
                stream,
                control == peerMixControls_.end() ? 1.0F : control->second.volume,
            };
        }
    }
    std::array<float, kOpusFrameSamples> decoded{};
    std::size_t mixed = 0;
    for (std::size_t index = 0; index < streamCount; ++index) {
        const auto& active = activeStreams[index];
        if (!active.stream || !active.stream->Decode(decoded.data())) continue;
        decodedOpusFrames_.fetch_add(1, std::memory_order_relaxed);
        for (int sample = 0; sample < kOpusFrameSamples; ++sample) {
            output[sample] += decoded[sample] * active.volume;
        }
        ++mixed;
    }
    if (mixed == 0) return;
    if (outputMuted_.load()) {
        std::fill(output, output + kOpusFrameSamples, 0.0F);
        return;
    }
    const float gain = masterOutputVolume_.load() / std::sqrt(static_cast<float>(mixed));
    float peak = 0.0F;
    for (int sample = 0; sample < kOpusFrameSamples; ++sample) {
        peak = std::max(peak, std::abs(output[sample] * gain));
    }
    mixLimiterGain_ = AdvanceBlockLimiterGain(mixLimiterGain_, peak);
    const float limitedGain = gain * mixLimiterGain_;
    for (int sample = 0; sample < kOpusFrameSamples; ++sample) {
        output[sample] = std::clamp(output[sample] * limitedGain, -1.0F, 1.0F);
    }
}

void AudioEngine::RenderLoop() {
    // Keep the selected WASAPI client alive for the lifetime of the audio
    // engine. Reopening the endpoint after every short conversational pause
    // caused a visible/audio hitch exactly when the next speaker started.
    RunRenderSession(true);
}

void AudioEngine::RunRenderSession(const bool startupProbe) {
    const auto fail = [this, startupProbe](std::string error) {
        if (startupProbe) CompleteStartup(false, false, std::move(error));
        else SetError(std::move(error));
    };
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult)) {
        fail(HResultText("Render COM baslatilamadi", comResult));
        return;
    }
    SetThreadDescription(GetCurrentThread(), L"Sonalis Render");
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (eventHandle == nullptr) {
        fail("Render olay taniticisi olusturulamadi");
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
        return;
    }

    HRESULT result = S_OK;
    ComPtr<IMMDevice> device = ResolveDevice(eRender, outputDeviceId_, result);
    ComPtr<IAudioClient3> client;
    UINT32 bufferFrames = 0;
    std::string error;
    if (!device || FAILED(result) || !InitializeSharedClient(device, client, eventHandle, bufferFrames, error)) {
        fail(device ? error : HResultText("Cikis cihazi acilamadi", result));
        CloseHandle(eventHandle);
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
        return;
    }
    ComPtr<IAudioRenderClient> render;
    result = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(result)) {
        fail(HResultText("Render servisi alinamadi", result));
        CloseHandle(eventHandle);
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
        return;
    }
    const auto primeBuffer = [&]() -> HRESULT {
        BYTE* initial = nullptr;
        const HRESULT getResult = render->GetBuffer(bufferFrames, &initial);
        if (FAILED(getResult)) return getResult;
        return render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    };
    result = primeBuffer();
    if (SUCCEEDED(result)) result = client->Start();
    if (FAILED(result)) {
        fail(HResultText("Render akisi baslatilamadi", result));
        CloseHandle(eventHandle);
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
        return;
    }
    result = client->Stop();
    if (SUCCEEDED(result)) result = client->Reset();
    if (FAILED(result)) {
        fail(HResultText("Render akisi dogrulanamadi", result));
        CloseHandle(eventHandle);
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
        return;
    }
    if (startupProbe) CompleteStartup(false, true);

    std::array<float, kOpusFrameSamples> mixBlock{};
    bool clientStarted = false;
    while (running_.load()) {
        {
            std::unique_lock lock(renderWakeMutex_);
            renderWakeCv_.wait(lock, [this] {
                return !running_.load() || lastIncomingAudioMs_.load() != 0;
            });
        }
        if (!running_.load()) break;

        result = primeBuffer();
        if (SUCCEEDED(result)) result = client->Start();
        if (FAILED(result)) {
            SetError(HResultText("Render akisi yeniden baslatilamadi", result));
            lastIncomingAudioMs_.store(0);
            break;
        }
        clientStarted = true;
        int mixOffset = kOpusFrameSamples;

        while (running_.load()) {
            std::uint64_t lastAudio = lastIncomingAudioMs_.load();
            if (lastAudio == 0 || SteadyMilliseconds() - lastAudio >= 2'500) {
                const std::uint64_t closingGeneration = renderGeneration_.load();
                client->Stop();
                clientStarted = false;
                client->Reset();
                if (lastAudio != 0 && renderGeneration_.load() == closingGeneration) {
                    lastIncomingAudioMs_.compare_exchange_strong(lastAudio, 0);
                }
                break;
            }
            if (WaitForSingleObject(eventHandle, 100) != WAIT_OBJECT_0) continue;
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding)) || padding >= bufferFrames) continue;
            const UINT32 available = bufferFrames - padding;
            BYTE* raw = nullptr;
            if (FAILED(render->GetBuffer(available, &raw)) || raw == nullptr) continue;
            float* destination = reinterpret_cast<float*>(raw);
            for (UINT32 index = 0; index < available; ++index) {
                if (mixOffset >= kOpusFrameSamples) {
                    MixNextBlock(mixBlock.data());
                    mixOffset = 0;
                }
                destination[index] = mixBlock[mixOffset++];
            }
            render->ReleaseBuffer(available, 0);
            wasapiRenderFrames_.fetch_add(available, std::memory_order_relaxed);
        }
    }

    if (clientStarted) client->Stop();
    CloseHandle(eventHandle);
    if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

}  // namespace ss
