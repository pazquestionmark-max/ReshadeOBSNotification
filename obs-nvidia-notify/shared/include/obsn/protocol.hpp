// SPDX-License-Identifier: MIT
// Versioned message envelope and the payloads carried in it. See docs/protocol.md.
//
// One line of JSON per message, newline-delimited, with a hard size cap. The vocabulary is
// small on purpose: the producer sends a snapshot of what is true and a stream of what changed,
// and the consumer says hello and pings. Everything else is a property of those two shapes.
#ifndef OBSN_PROTOCOL_HPP
#define OBSN_PROTOCOL_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "obsn/model.hpp"

namespace obsn::proto {

inline constexpr int kProtocolVersion = 1;
inline constexpr int kProtocolMin = 1;
inline constexpr int kProtocolMax = 1;
inline constexpr std::size_t kMaxMessageBytes = 65536;
inline constexpr std::size_t kMaxDetailChars = 512;
inline constexpr std::size_t kMaxPathChars = 1024;
inline constexpr int kHeartbeatIntervalMs = 2000;

enum class MessageType {
    Unknown,
    // producer (the OBS script) -> consumer (the overlay)
    Hello,
    StateSnapshot,
    Event,
    Heartbeat,
    Error,
    // consumer -> producer
    ClientHello,
    RequestSnapshot,
    Ping,
    Pong,
};

const char* to_string(MessageType) noexcept;
MessageType parse_message_type(std::string_view) noexcept;
/// Whether this type may legitimately travel producer -> consumer. A consumer that receives a
/// consumer-only type is talking to something that is not an OBS script, and says so rather
/// than trying to make sense of it.
bool is_producer_to_consumer(MessageType) noexcept;

/// The envelope every message shares.
struct Envelope {
    int version = kProtocolVersion;
    std::int64_t seq = 0;
    std::int64_t ts = 0;
    MessageType type = MessageType::Unknown;
    json::Value data;   ///< object; may be null or absent

    std::string encode() const;
};

enum class DecodeStatus {
    Ok,
    NotJson,
    NotObject,
    MissingField,
    BadVersion,
    UnknownType,
    TooLarge,
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::NotJson;
    Envelope envelope;
    std::string error;
    bool ok() const noexcept { return status == DecodeStatus::Ok; }
    /// Whether this failure means the peer is malformed enough to warrant dropping the
    /// connection, as opposed to one droppable message.
    bool fatal() const noexcept {
        return status == DecodeStatus::NotJson || status == DecodeStatus::TooLarge ||
               status == DecodeStatus::NotObject;
    }
};

DecodeResult decode(std::string_view line);

// --- payloads ---------------------------------------------------------------------------------

struct HelloPayload {
    int protocol_min = kProtocolMin;
    int protocol_max = kProtocolMax;
    std::string script_version;
    std::string obs_version;
    std::string platform;
    std::vector<std::string> capabilities;

    json::Value to_json() const;
    static HelloPayload from_json(const json::Value&);
};

struct ClientHelloPayload {
    int protocol = kProtocolVersion;
    std::string client = "reshade-addon";
    std::string client_version;
    std::string process;   ///< the game executable, so the producer's log names the right game
    std::int64_t pid = 0;

    json::Value to_json() const;
    static ClientHelloPayload from_json(const json::Value&);
};

struct ErrorPayload {
    std::string code;
    std::string message;

    json::Value to_json() const;
    static ErrorPayload from_json(const json::Value&);
};

/// Builds a complete, encodable message. `seq` is the producer's monotonic counter.
Envelope make(MessageType type, json::Value data, std::int64_t seq, std::int64_t ts);

/// Clamps the free-text and path fields of an incoming event to the protocol's limits.
///
/// Applied on the *consuming* side as well as the producing one: a peer that ignores the limit
/// must not be able to put a megabyte of text through the layout engine, and "the other end
/// promised not to" is not a bound.
void clamp_event(ObsEvent& event);

}  // namespace obsn::proto

#endif  // OBSN_PROTOCOL_HPP
