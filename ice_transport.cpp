#include "ice_transport.h"

#include <array>
#include <chrono>
#include <utility>

namespace ss {

IceTransport::~IceTransport() { Stop(); }

bool IceTransport::Start(const std::string& stunHost, const std::uint16_t stunPort,
                         ReceiveCallback receive, std::string& error) {
    Stop();
    if (stunHost.empty() || stunPort == 0) {
        error = "STUN adresi yapilandirilmamis";
        return false;
    }
    stunHost_ = stunHost;
    receive_ = std::move(receive);
    gatheringDone_.store(false);
    connected_.store(false);
    juice_config_t config{};
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
    config.stun_server_host = stunHost_.c_str();
    config.stun_server_port = stunPort;
    config.cb_state_changed = &IceTransport::StateChanged;
    config.cb_gathering_done = &IceTransport::GatheringDone;
    config.cb_recv = &IceTransport::Received;
    config.user_ptr = this;
    {
        std::scoped_lock lock(mutex_);
        agent_ = juice_create(&config);
    }
    if (agent_ == nullptr) {
        error = "ICE agenti olusturulamadi";
        return false;
    }
    if (juice_gather_candidates(agent_) != JUICE_ERR_SUCCESS) {
        error = "ICE adaylari toplanamadi";
        Stop();
        return false;
    }
    {
        std::unique_lock lock(mutex_);
        if (!stateChanged_.wait_for(lock, std::chrono::seconds(3), [this] { return gatheringDone_.load(); })) {
            error = "STUN aday toplama zaman asimi";
            lock.unlock();
            Stop();
            return false;
        }
        std::array<char, JUICE_MAX_SDP_STRING_LEN> description{};
        if (juice_get_local_description(agent_, description.data(), description.size()) != JUICE_ERR_SUCCESS) {
            error = "Yerel ICE aciklamasi alinamadi";
            lock.unlock();
            Stop();
            return false;
        }
        localDescription_ = description.data();
    }
    return !localDescription_.empty();
}

bool IceTransport::SetRemoteDescription(const std::string& description, std::string& error) {
    if (description.empty() || description.size() >= JUICE_MAX_SDP_STRING_LEN) {
        error = "Uzak ICE aciklamasi gecersiz";
        return false;
    }
    std::scoped_lock lock(mutex_);
    if (agent_ == nullptr || juice_set_remote_description(agent_, description.c_str()) != JUICE_ERR_SUCCESS) {
        error = "Uzak ICE aciklamasi uygulanamadi";
        return false;
    }
    return true;
}

bool IceTransport::Send(const std::span<const std::uint8_t> datagram) noexcept {
    if (datagram.empty() || datagram.size() > 1500 || !connected_.load()) return false;
    try {
        std::scoped_lock lock(mutex_);
        return agent_ != nullptr && juice_send(agent_, reinterpret_cast<const char*>(datagram.data()), datagram.size()) == JUICE_ERR_SUCCESS;
    } catch (...) { return false; }
}

void IceTransport::Stop() noexcept {
    juice_agent_t* agent = nullptr;
    {
        std::scoped_lock lock(mutex_);
        agent = std::exchange(agent_, nullptr);
        localDescription_.clear();
    }
    if (agent != nullptr) juice_destroy(agent);
    receive_ = {};
    gatheringDone_.store(false);
    connected_.store(false);
    stateChanged_.notify_all();
}

bool IceTransport::IsStarted() const noexcept {
    std::scoped_lock lock(mutex_);
    return agent_ != nullptr;
}

bool IceTransport::IsConnected() const noexcept { return connected_.load(); }

std::string IceTransport::LocalDescription() const {
    std::scoped_lock lock(mutex_);
    return localDescription_;
}

void IceTransport::StateChanged(juice_agent_t*, const juice_state_t state, void* user) noexcept {
    auto* transport = static_cast<IceTransport*>(user);
    if (transport == nullptr) return;
    transport->connected_.store(state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED);
    transport->stateChanged_.notify_all();
}

void IceTransport::GatheringDone(juice_agent_t*, void* user) noexcept {
    auto* transport = static_cast<IceTransport*>(user);
    if (transport == nullptr) return;
    transport->gatheringDone_.store(true);
    transport->stateChanged_.notify_all();
}

void IceTransport::Received(juice_agent_t*, const char* data, const std::size_t size, void* user) noexcept {
    auto* transport = static_cast<IceTransport*>(user);
    if (transport == nullptr || data == nullptr || size == 0 || size > 1500) return;
    if (transport->receive_) {
        transport->receive_(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data), size));
    }
}

}  // namespace ss
