// SPDX-License-Identifier: MIT
#include "obsn/protocol.hpp"
#include "obsn_test.hpp"

using namespace obsn;
using namespace obsn::proto;

TEST(protocol, an_envelope_round_trips) {
    ObsEvent event;
    event.kind = EventKind::ReplayBufferSaved;
    event.path = "Replay.mkv";
    event.replay_seconds = 30;

    const Envelope sent = make(MessageType::Event, to_json(event), 7, 1'700'000'000'000);
    const DecodeResult decoded = decode(sent.encode());
    CHECK(decoded.ok());
    CHECK(decoded.envelope.type == MessageType::Event);
    CHECK_EQ(decoded.envelope.seq, 7);
    CHECK_EQ(decoded.envelope.ts, 1'700'000'000'000);

    const ObsEvent back = event_from_json(decoded.envelope.data);
    CHECK(back.kind == EventKind::ReplayBufferSaved);
    CHECK_EQ(back.path, std::string("Replay.mkv"));
}

TEST(protocol, rubbish_is_rejected_without_crashing) {
    CHECK(!decode("").ok());
    CHECK(!decode("not json at all").ok());
    CHECK(!decode("[1,2,3]").ok());
    CHECK(!decode("{}").ok());
    CHECK(decode("[1,2,3]").status == DecodeStatus::NotObject);
    CHECK(decode("{}").status == DecodeStatus::MissingField);
}

TEST(protocol, an_unsupported_version_is_refused_before_the_type_is_read) {
    // A peer speaking a protocol we do not know may well use type names that mean something
    // else entirely, so the version is the first thing checked.
    const DecodeResult decoded = decode(R"({"v":99,"type":"event"})");
    CHECK(!decoded.ok());
    CHECK(decoded.status == DecodeStatus::BadVersion);
}

TEST(protocol, an_unknown_type_is_droppable_not_fatal) {
    // A newer producer sending a type this build does not know is a message to skip, not a
    // reason to drop a working connection.
    const DecodeResult decoded = decode(R"({"v":1,"type":"something_new"})");
    CHECK(!decoded.ok());
    CHECK(decoded.status == DecodeStatus::UnknownType);
    CHECK(!decoded.fatal());
}

TEST(protocol, malformed_json_is_fatal) {
    CHECK(decode("{\"v\":1,").fatal());
}

TEST(protocol, an_over_long_message_is_refused_before_it_is_parsed) {
    const std::string huge(kMaxMessageBytes + 10, 'x');
    const DecodeResult decoded = decode(huge);
    CHECK(decoded.status == DecodeStatus::TooLarge);
    CHECK(decoded.fatal());
}

TEST(protocol, direction_is_enforced) {
    CHECK(is_producer_to_consumer(MessageType::Event));
    CHECK(is_producer_to_consumer(MessageType::StateSnapshot));
    CHECK(is_producer_to_consumer(MessageType::Hello));
    CHECK(is_producer_to_consumer(MessageType::Pong));
    // A peer echoing these back is not an OBS script.
    CHECK(!is_producer_to_consumer(MessageType::ClientHello));
    CHECK(!is_producer_to_consumer(MessageType::Ping));
    CHECK(!is_producer_to_consumer(MessageType::RequestSnapshot));
}

TEST(protocol, every_type_round_trips_through_its_name) {
    const MessageType all[] = {
        MessageType::Hello, MessageType::StateSnapshot, MessageType::Event,
        MessageType::Heartbeat, MessageType::Error, MessageType::ClientHello,
        MessageType::RequestSnapshot, MessageType::Ping, MessageType::Pong,
    };
    for (const MessageType type : all) {
        CHECK(parse_message_type(to_string(type)) == type);
    }
    CHECK(parse_message_type("nonsense") == MessageType::Unknown);
}

TEST(protocol, the_handshake_payloads_round_trip) {
    HelloPayload hello;
    hello.script_version = "1.0.0";
    hello.obs_version = "30.2.3";
    hello.platform = "Windows";
    hello.capabilities = {"recording", "replay_buffer"};
    const HelloPayload back = HelloPayload::from_json(hello.to_json());
    CHECK_EQ(back.obs_version, std::string("30.2.3"));
    CHECK_EQ(back.capabilities.size(), std::size_t{2});

    ClientHelloPayload client;
    client.process = "game.exe";
    client.pid = 4321;
    const ClientHelloPayload client_back = ClientHelloPayload::from_json(client.to_json());
    CHECK_EQ(client_back.process, std::string("game.exe"));
    CHECK_EQ(client_back.pid, 4321);
}

TEST(protocol, incoming_events_are_clamped_on_the_receiving_side_too) {
    // "The other end promised not to" is not a bound. A peer that ignores the limit must not be
    // able to put a megabyte of text through the layout engine.
    ObsEvent event;
    event.detail = std::string(kMaxDetailChars * 4, 'x');
    event.path = std::string(kMaxPathChars * 4, 'p');
    event.duration_ms = -50;
    event.attempt = -3;
    clamp_event(event);
    CHECK(event.detail.size() <= kMaxDetailChars);
    CHECK(event.path.size() <= kMaxPathChars);
    // A negative duration normalises to the "not known" sentinel rather than being formatted.
    CHECK_EQ(event.duration_ms, -1);
    CHECK_EQ(event.attempt, 0);
}

TEST(protocol, clamping_never_splits_a_utf8_sequence) {
    ObsEvent event;
    // Three-byte characters, so a byte-wise truncation would land mid-sequence.
    std::string detail;
    for (std::size_t i = 0; i < kMaxDetailChars * 2; ++i) detail += "\xE2\x9C\x93";
    event.detail = detail;
    clamp_event(event);
    CHECK(json::is_valid_utf8(event.detail));
}
