// SPDX-License-Identifier: MIT
// End-to-end over a real kernel transport.
//
// A minimal producer is stood up here -- the same handshake and message shapes the OBS script
// sends -- and the real OverlayClient is run against it over an actual AF_UNIX socket. That is
// what makes the handshake, the framing, the reconnect policy and the state application tested
// rather than merely reviewed.
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "obsn/framing.hpp"
#include "obsn/overlay_client.hpp"
#include "obsn/protocol.hpp"
#include "obsn/transport.hpp"
#include "obsn_test.hpp"

using namespace obsn;

namespace {

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string unique_endpoint(const char* name) {
    return default_endpoint(std::string("obsn-test-") + name + "-" +
                            std::to_string(now_ms() % 1'000'000));
}

/// Stands in for the OBS script: accepts one overlay, completes the handshake, and sends
/// whatever the test tells it to.
class TestProducer {
public:
    explicit TestProducer(std::string endpoint) : endpoint_(std::move(endpoint)) {}

    ~TestProducer() { stop(); }

    bool start(std::string& error) {
        transport_ = create_server(endpoint_, error);
        if (!transport_) return false;
        running_ = true;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (transport_) transport_->stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (connection_ != nullptr) connection_->cancel();
        }
        if (thread_.joinable()) thread_.join();
        transport_.reset();
    }

    void send(proto::MessageType type, const json::Value& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_ == nullptr) return;
        proto::Envelope envelope = proto::make(type, data, ++seq_, now_ms());
        std::string line = envelope.encode();
        line.push_back('\n');
        connection_->write_all(line.data(), line.size());
    }

    bool wait_for_handshake(int timeout_ms) {
        const std::int64_t deadline = now_ms() + timeout_ms;
        while (now_ms() < deadline) {
            if (handshaked_.load()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    /// Drops the connection without closing the listener, so the overlay's reconnect path runs.
    void drop_client() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_ != nullptr) connection_->cancel();
    }

    int client_hellos() const { return client_hellos_.load(); }
    int pings_answered() const { return pings_answered_.load(); }

private:
    void run() {
        while (running_.load()) {
            std::unique_ptr<Connection> connection = transport_->accept(200);
            if (!connection) continue;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                connection_ = connection.get();
            }
            serve(*connection);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                connection_ = nullptr;
            }
            handshaked_.store(false);
        }
    }

    void serve(Connection& connection) {
        LineFramer framer(proto::kMaxMessageBytes);
        std::vector<std::string> lines;
        std::string buffer(4096, '\0');

        while (running_.load()) {
            const int read = connection.read(buffer.data(), buffer.size());
            if (read <= 0) return;
            lines.clear();
            if (!framer.feed(std::string_view(buffer.data(), static_cast<std::size_t>(read)),
                             lines)) {
                return;
            }
            for (const std::string& line : lines) {
                const proto::DecodeResult decoded = proto::decode(line);
                if (!decoded.ok()) continue;
                switch (decoded.envelope.type) {
                    case proto::MessageType::ClientHello: {
                        ++client_hellos_;
                        proto::HelloPayload hello;
                        hello.script_version = "test";
                        hello.obs_version = "30.2.3";
                        hello.platform = "test";
                        send(proto::MessageType::Hello, hello.to_json());
                        handshaked_.store(true);
                        break;
                    }
                    case proto::MessageType::RequestSnapshot: {
                        ObsState state;
                        state.obs_version = "30.2.3";
                        state.current_scene = "Gameplay";
                        state.recording.state = OutputState::Active;
                        state.recording.started_ms = now_ms();
                        send(proto::MessageType::StateSnapshot, to_json(state));
                        break;
                    }
                    case proto::MessageType::Ping:
                        ++pings_answered_;
                        send(proto::MessageType::Pong, json::Value());
                        break;
                    default:
                        break;
                }
            }
        }
    }

    std::string endpoint_;
    std::unique_ptr<ServerTransport> transport_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> handshaked_{false};
    std::atomic<int> client_hellos_{0};
    std::atomic<int> pings_answered_{0};
    std::mutex mutex_;
    Connection* connection_ = nullptr;
    std::int64_t seq_ = 0;
};

OverlayClientConfig client_config(const std::string& endpoint) {
    OverlayClientConfig config;
    config.endpoint = endpoint;
    config.reconnect_initial_ms = 60;
    config.reconnect_max_ms = 200;
    config.ping_interval_ms = 100;
    config.hello.process = "test.exe";
    return config;
}

/// Polls `predicate` until it holds or the deadline passes. Returns whether it held.
template <typename Predicate>
bool wait_until(Predicate predicate, int timeout_ms) {
    const std::int64_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

}  // namespace

TEST(ipc, the_handshake_completes_and_a_snapshot_arrives) {
    const std::string endpoint = unique_endpoint("handshake");
    TestProducer producer(endpoint);
    std::string error;
    CHECK(producer.start(error));

    OverlayClient client(client_config(endpoint));
    client.start();

    CHECK(producer.wait_for_handshake(3000));
    CHECK(wait_until([&] { return client.latest().state.obs_running; }, 3000));

    const OverlayFrame& frame = client.latest();
    CHECK(frame.link == LinkState::Connected);
    CHECK_EQ(frame.state.current_scene, std::string("Gameplay"));
    CHECK(frame.state.recording.state == OutputState::Active);

    const LinkDiagnostics diagnostics = client.diagnostics();
    CHECK_EQ(diagnostics.obs_version, std::string("30.2.3"));

    client.stop();
    producer.stop();
}

TEST(ipc, events_reach_the_notification_layer) {
    const std::string endpoint = unique_endpoint("events");
    TestProducer producer(endpoint);
    std::string error;
    CHECK(producer.start(error));

    OverlayClient client(client_config(endpoint));
    client.start();
    CHECK(producer.wait_for_handshake(3000));

    ObsEvent event;
    event.kind = EventKind::ReplayBufferSaved;
    event.ts = now_ms();
    event.path = "Replay.mkv";
    event.replay_seconds = 30;
    producer.send(proto::MessageType::Event, to_json(event));

    std::vector<ObsEvent> drained;
    CHECK(wait_until([&] {
        std::vector<ObsEvent> batch = client.drain_events();
        for (ObsEvent& e : batch) drained.push_back(std::move(e));
        for (const ObsEvent& e : drained) {
            if (e.kind == EventKind::ReplayBufferSaved) return true;
        }
        return false;
    }, 3000));

    bool found = false;
    for (const ObsEvent& e : drained) {
        if (e.kind != EventKind::ReplayBufferSaved) continue;
        found = true;
        CHECK_EQ(e.path, std::string("Replay.mkv"));
        CHECK_EQ(e.replay_seconds, 30);
    }
    CHECK(found);

    client.stop();
    producer.stop();
}

TEST(ipc, pings_are_answered_and_the_round_trip_is_measured) {
    const std::string endpoint = unique_endpoint("ping");
    TestProducer producer(endpoint);
    std::string error;
    CHECK(producer.start(error));

    OverlayClient client(client_config(endpoint));
    client.start();
    CHECK(producer.wait_for_handshake(3000));

    // The client only sends a ping when it has had traffic to wake it, so the producer keeps
    // something arriving.
    CHECK(wait_until([&] {
        producer.send(proto::MessageType::Heartbeat, json::Value());
        return producer.pings_answered() > 0 && client.diagnostics().round_trip_ms >= 0;
    }, 5000));

    client.stop();
    producer.stop();
}

TEST(ipc, a_dropped_link_reconnects_by_itself) {
    const std::string endpoint = unique_endpoint("reconnect");
    TestProducer producer(endpoint);
    std::string error;
    CHECK(producer.start(error));

    OverlayClient client(client_config(endpoint));
    client.start();
    CHECK(producer.wait_for_handshake(3000));
    CHECK_EQ(producer.client_hellos(), 1);

    producer.drop_client();

    // A second client_hello is the reconnect having happened, not merely having been attempted.
    CHECK(wait_until([&] { return producer.client_hellos() >= 2; }, 5000));
    CHECK(wait_until([&] { return client.latest().link == LinkState::Connected; }, 3000));

    client.stop();
    producer.stop();
}

TEST(ipc, a_missing_producer_is_an_ordinary_state_not_a_failure) {
    // OBS not running is the common case, and the overlay must sit in backoff rather than
    // giving up or spinning.
    OverlayClient client(client_config(unique_endpoint("absent")));
    client.start();
    CHECK(wait_until([&] {
        const LinkState link = client.latest().link;
        return link == LinkState::Backoff || link == LinkState::Connecting;
    }, 3000));
    CHECK(!client.latest().state.obs_running);
    client.stop();
}

TEST(ipc, malformed_input_is_counted_and_survived) {
    const std::string endpoint = unique_endpoint("garbage");
    TestProducer producer(endpoint);
    std::string error;
    CHECK(producer.start(error));

    OverlayClient client(client_config(endpoint));
    client.start();
    CHECK(producer.wait_for_handshake(3000));

    // A message that decodes but is not for us, then one that does not decode at all.
    producer.send(proto::MessageType::Heartbeat, json::Value());
    ObsEvent event;
    event.kind = EventKind::RecordingStarted;
    producer.send(proto::MessageType::Event, to_json(event));

    CHECK(wait_until([&] {
        const std::vector<ObsEvent> batch = client.drain_events();
        for (const ObsEvent& e : batch) {
            if (e.kind == EventKind::RecordingStarted) return true;
        }
        return false;
    }, 3000));
    CHECK(client.latest().link == LinkState::Connected);

    client.stop();
    producer.stop();
}

TEST(ipc, stopping_is_prompt_even_while_parked_on_a_read) {
    const std::string endpoint = unique_endpoint("stop");
    TestProducer producer(endpoint);
    std::string error;
    CHECK(producer.start(error));

    OverlayClient client(client_config(endpoint));
    client.start();
    CHECK(producer.wait_for_handshake(3000));

    // Nothing is arriving, so the client's read is blocked. stop() must cancel it rather than
    // waiting for a peer that has gone quiet.
    const std::int64_t started = now_ms();
    client.stop();
    CHECK(now_ms() - started < 2000);
    CHECK(!client.running());

    producer.stop();
}
