// SPDX-License-Identifier: MIT
#include "obsn/overlay_client.hpp"

#include <algorithm>
#include <chrono>

#include "obsn/framing.hpp"
#include "obsn/log.hpp"

namespace obsn {
namespace {

constexpr char kComponent[] = "link";

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

const char* to_string(LinkState state) noexcept {
    switch (state) {
        case LinkState::Idle: return "idle";
        case LinkState::Connecting: return "connecting";
        case LinkState::Handshaking: return "handshaking";
        case LinkState::Connected: return "connected";
        case LinkState::Backoff: return "waiting to retry";
        case LinkState::Failed: return "failed";
    }
    return "unknown";
}

OverlayClient::OverlayClient(OverlayClientConfig config) : config_(std::move(config)) {
    endpoint_ = config_.endpoint.empty() ? default_endpoint("") : config_.endpoint;
    stale_after_ms_.store(config_.stale_after_ms, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(diag_mutex_);
    diag_.endpoint = endpoint_;
}

OverlayClient::~OverlayClient() { stop(); }

void OverlayClient::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    thread_ = std::thread([this] { run(); });
}

void OverlayClient::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    {
        // Cancelling the live connection is what unblocks a read that is parked waiting for a
        // message that will never come. Without it, stop() would wait for the peer.
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (active_connection_ != nullptr) active_connection_->cancel();
    }
    wake_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    set_link(LinkState::Idle, "stopped");
}

void OverlayClient::set_endpoint(const std::string& endpoint) {
    {
        std::lock_guard<std::mutex> lock(diag_mutex_);
        endpoint_ = endpoint.empty() ? default_endpoint("") : endpoint;
        diag_.endpoint = endpoint_;
    }
    request_reconnect();
}

void OverlayClient::set_stale_after_ms(int ms) {
    stale_after_ms_.store(std::max(500, ms), std::memory_order_relaxed);
}

void OverlayClient::request_reconnect() {
    reconnect_requested_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (active_connection_ != nullptr) active_connection_->cancel();
    }
    wake_cv_.notify_all();
}

void OverlayClient::set_link(LinkState state, std::string detail) {
    std::lock_guard<std::mutex> lock(diag_mutex_);
    diag_.state = state;
    diag_.detail = std::move(detail);
}

void OverlayClient::publish(LinkState link, std::string detail, bool stale) {
    OverlayFrame& frame = frames_.write_slot();
    frame.state = state_;
    frame.link = link;
    frame.link_detail = std::move(detail);
    frame.stale = stale;
    frame.published_ms = now_ms();
    frame.revision = ++revision_;
    frames_.publish();
}

void OverlayClient::push_event(const ObsEvent& event) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    // A bounded queue: the renderer drains this once a frame, so it only grows if the game is
    // not drawing. Dropping the oldest is right here -- the newest event is the one that
    // describes the world as it is now.
    if (pending_events_.size() >= config_.max_pending_events) {
        pending_events_.pop_front();
        std::lock_guard<std::mutex> diag_lock(diag_mutex_);
        ++diag_.events_dropped;
    }
    pending_events_.push_back(event);
}

std::vector<ObsEvent> OverlayClient::drain_events() {
    std::lock_guard<std::mutex> lock(events_mutex_);
    std::vector<ObsEvent> out(pending_events_.begin(), pending_events_.end());
    pending_events_.clear();
    return out;
}

LinkDiagnostics OverlayClient::diagnostics() const {
    std::lock_guard<std::mutex> lock(diag_mutex_);
    return diag_;
}

bool OverlayClient::send(Connection& connection, proto::MessageType type,
                         const json::Value& data) {
    proto::Envelope envelope = proto::make(type, data, ++client_seq_, now_ms());
    std::string line = envelope.encode();
    line.push_back('\n');
    return connection.write_all(line.data(), line.size());
}

void OverlayClient::run() {
    int backoff = config_.reconnect_initial_ms;
    int attempts = 0;

    while (running_.load(std::memory_order_acquire)) {
        reconnect_requested_.store(false, std::memory_order_release);

        std::string endpoint;
        {
            std::lock_guard<std::mutex> lock(diag_mutex_);
            endpoint = endpoint_;
        }

        set_link(LinkState::Connecting, "connecting to " + endpoint);
        publish(LinkState::Connecting, "connecting", false);

        std::string error;
        std::unique_ptr<Connection> connection = connect_client(endpoint, 1000, error);
        if (!connection) {
            ++attempts;
            {
                std::lock_guard<std::mutex> lock(diag_mutex_);
                diag_.reconnect_attempts = attempts;
                diag_.next_retry_in_ms = backoff;
            }
            // OBS not running, or the script not loaded, is the ordinary case rather than a
            // fault: it is logged at debug so a real failure is not buried in it.
            OBSN_DEBUG(kComponent, "not connected: " + error);
            set_link(LinkState::Backoff, error);
            publish(LinkState::Backoff, error, false);

            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(backoff), [this] {
                return !running_.load(std::memory_order_acquire) ||
                       reconnect_requested_.load(std::memory_order_acquire);
            });
            // Exponential with a ceiling: a user who starts OBS an hour in should still see the
            // overlay attach within a few seconds, not after a backoff that grew all session.
            backoff = std::min(config_.reconnect_max_ms, backoff * 2);
            continue;
        }

        attempts = 0;
        backoff = config_.reconnect_initial_ms;
        session(std::move(connection));

        if (!running_.load(std::memory_order_acquire)) break;

        // A link that dropped immediately after connecting waits the initial backoff before
        // trying again, so a producer that accepts and instantly closes cannot be hammered.
        std::unique_lock<std::mutex> lock(wake_mutex_);
        wake_cv_.wait_for(lock, std::chrono::milliseconds(config_.reconnect_initial_ms), [this] {
            return !running_.load(std::memory_order_acquire) ||
                   reconnect_requested_.load(std::memory_order_acquire);
        });
    }

    state_ = ObsState{};
    publish(LinkState::Idle, "stopped", false);
}

void OverlayClient::session(std::unique_ptr<Connection> connection) {
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        active_connection_ = connection.get();
    }
    // The pointer is cleared on every exit path, including an exception, because the connection
    // it refers to is destroyed the moment this function returns.
    struct ClearOnExit {
        OverlayClient* self;
        ~ClearOnExit() {
            std::lock_guard<std::mutex> lock(self->connection_mutex_);
            self->active_connection_ = nullptr;
        }
    } clear{this};

    set_link(LinkState::Handshaking, "sending client_hello");
    if (!send(*connection, proto::MessageType::ClientHello, config_.hello.to_json())) {
        set_link(LinkState::Failed, "could not send client_hello");
        return;
    }

    const std::int64_t connected_at = now_ms();
    {
        std::lock_guard<std::mutex> lock(diag_mutex_);
        diag_.connected_since_ms = connected_at;
        diag_.round_trip_ms = -1;
    }

    LineFramer framer(proto::kMaxMessageBytes);
    std::vector<std::string> lines;
    std::string buffer(8192, '\0');

    std::int64_t last_message = connected_at;
    std::int64_t last_ping = connected_at;
    std::int64_t ping_sent_at = 0;
    bool handshaked = false;
    bool stale = false;

    while (running_.load(std::memory_order_acquire) &&
           !reconnect_requested_.load(std::memory_order_acquire)) {
        const int read = connection->read(buffer.data(), buffer.size());
        if (read == 0) {
            OBSN_INFO(kComponent, "the OBS script closed the connection");
            break;
        }
        if (read < 0) {
            if (running_.load(std::memory_order_acquire) &&
                !reconnect_requested_.load(std::memory_order_acquire)) {
                OBSN_INFO(kComponent, "connection lost");
            }
            break;
        }

        lines.clear();
        if (!framer.feed(std::string_view(buffer.data(), static_cast<std::size_t>(read)), lines)) {
            OBSN_WARN(kComponent, "peer sent an over-long line; dropping the connection");
            break;
        }

        const std::int64_t at = now_ms();
        bool fatal = false;
        for (const std::string& line : lines) {
            if (line.empty()) continue;
            const proto::DecodeResult decoded = proto::decode(line);
            {
                std::lock_guard<std::mutex> lock(diag_mutex_);
                ++diag_.messages_received;
                if (!decoded.ok()) ++diag_.malformed_messages;
            }
            if (!decoded.ok()) {
                OBSN_WARN(kComponent, "undecodable message: " + decoded.error);
                if (decoded.fatal()) {
                    fatal = true;
                    break;
                }
                continue;
            }
            const proto::Envelope& envelope = decoded.envelope;
            if (!proto::is_producer_to_consumer(envelope.type)) {
                // The peer is echoing consumer messages back, which means it is not an OBS
                // script. Saying so beats silently ignoring it while the overlay shows nothing.
                OBSN_WARN(kComponent, std::string("peer sent a consumer-only message type: ") +
                                          proto::to_string(envelope.type));
                continue;
            }

            last_message = at;
            switch (envelope.type) {
                case proto::MessageType::Hello: {
                    const proto::HelloPayload hello =
                        proto::HelloPayload::from_json(envelope.data);
                    if (hello.protocol_max < proto::kProtocolMin ||
                        hello.protocol_min > proto::kProtocolMax) {
                        OBSN_WARN(kComponent, "the OBS script speaks an incompatible protocol");
                        set_link(LinkState::Failed, "incompatible protocol version");
                        fatal = true;
                        break;
                    }
                    handshaked = true;
                    {
                        std::lock_guard<std::mutex> lock(diag_mutex_);
                        diag_.script_version = hello.script_version;
                        diag_.obs_version = hello.obs_version;
                        diag_.platform = hello.platform;
                        diag_.capabilities = hello.capabilities;
                    }
                    set_link(LinkState::Connected, "connected to OBS " + hello.obs_version);
                    // The snapshot is requested rather than assumed: a producer that sends one
                    // unprompted is fine, and one that waits to be asked is also fine.
                    send(*connection, proto::MessageType::RequestSnapshot, json::Value());
                    break;
                }
                case proto::MessageType::StateSnapshot:
                    state_ = state_from_json(envelope.data);
                    state_.obs_running = true;
                    state_.updated_ms = at;
                    break;
                case proto::MessageType::Event: {
                    ObsEvent event = event_from_json(envelope.data);
                    proto::clamp_event(event);
                    if (event.ts <= 0) event.ts = at;
                    // An event that carries a state replaces ours wholesale. That is how a
                    // producer resynchronises after something it could not express as an edge.
                    if (event.carries_state) {
                        state_ = event.state;
                        state_.obs_running = true;
                    }
                    state_.updated_ms = at;
                    push_event(event);
                    {
                        std::lock_guard<std::mutex> lock(diag_mutex_);
                        ++diag_.events_received;
                    }
                    break;
                }
                case proto::MessageType::Heartbeat:
                    // Nothing to do: `last_message` was already refreshed, which is the whole
                    // purpose of a heartbeat.
                    break;
                case proto::MessageType::Pong:
                    if (ping_sent_at > 0) {
                        std::lock_guard<std::mutex> lock(diag_mutex_);
                        diag_.round_trip_ms = at - ping_sent_at;
                        ping_sent_at = 0;
                    }
                    break;
                case proto::MessageType::Error: {
                    const proto::ErrorPayload error = proto::ErrorPayload::from_json(envelope.data);
                    OBSN_WARN(kComponent, "the OBS script reported: " + error.code + " " +
                                              error.message);
                    break;
                }
                default:
                    break;
            }
            if (fatal) break;
        }
        if (fatal) break;

        {
            std::lock_guard<std::mutex> lock(diag_mutex_);
            diag_.last_message_ms = last_message;
        }

        const int stale_after = stale_after_ms_.load(std::memory_order_relaxed);
        const bool now_stale = at - last_message > stale_after;
        if (now_stale != stale) {
            stale = now_stale;
            // Going stale is not a disconnect: the pipe is open and the producer may simply be
            // busy. The overlay stops presenting the state as live and says why.
            OBSN_INFO(kComponent, stale ? "no messages recently; marking the state stale"
                                        : "messages resumed");
        }

        if (config_.ping_interval_ms > 0 && at - last_ping >= config_.ping_interval_ms) {
            last_ping = at;
            ping_sent_at = at;
            if (!send(*connection, proto::MessageType::Ping, json::Value())) break;
        }

        publish(handshaked ? LinkState::Connected : LinkState::Handshaking,
                handshaked ? "connected" : "waiting for hello", stale);
    }

    // The link is down, so the state is no longer ours to present as true.
    state_.obs_running = false;
    publish(LinkState::Backoff, "disconnected", false);
    set_link(LinkState::Backoff, "disconnected");

    ObsEvent down;
    down.kind = EventKind::ScriptDisconnected;
    down.ts = now_ms();
    push_event(down);
}

}  // namespace obsn
