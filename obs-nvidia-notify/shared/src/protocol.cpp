// SPDX-License-Identifier: MIT
#include "obsn/protocol.hpp"

#include <algorithm>

namespace obsn::proto {
namespace {

struct TypeEntry {
    const char* name;
    MessageType type;
};

constexpr TypeEntry kTypes[] = {
    {"hello", MessageType::Hello},
    {"state_snapshot", MessageType::StateSnapshot},
    {"event", MessageType::Event},
    {"heartbeat", MessageType::Heartbeat},
    {"error", MessageType::Error},
    {"client_hello", MessageType::ClientHello},
    {"request_snapshot", MessageType::RequestSnapshot},
    {"ping", MessageType::Ping},
    {"pong", MessageType::Pong},
};

}  // namespace

const char* to_string(MessageType type) noexcept {
    for (const TypeEntry& e : kTypes) {
        if (e.type == type) return e.name;
    }
    return "unknown";
}

MessageType parse_message_type(std::string_view text) noexcept {
    for (const TypeEntry& e : kTypes) {
        if (text == e.name) return e.type;
    }
    return MessageType::Unknown;
}

bool is_producer_to_consumer(MessageType type) noexcept {
    switch (type) {
        case MessageType::Hello:
        case MessageType::StateSnapshot:
        case MessageType::Event:
        case MessageType::Heartbeat:
        case MessageType::Error:
        case MessageType::Pong:
            return true;
        case MessageType::ClientHello:
        case MessageType::RequestSnapshot:
        case MessageType::Ping:
        case MessageType::Unknown:
            return false;
    }
    return false;
}

std::string Envelope::encode() const {
    json::Object o;
    o.emplace_back("v", json::Value(version));
    o.emplace_back("seq", json::Value(seq));
    o.emplace_back("ts", json::Value(ts));
    o.emplace_back("type", json::Value(to_string(type)));
    if (!data.is_null()) o.emplace_back("data", data);
    return json::Value(std::move(o)).dump();
}

DecodeResult decode(std::string_view line) {
    DecodeResult result;
    if (line.size() > kMaxMessageBytes) {
        result.status = DecodeStatus::TooLarge;
        result.error = "message exceeds the " + std::to_string(kMaxMessageBytes) + " byte limit";
        return result;
    }

    json::Limits limits;
    limits.max_total_bytes = kMaxMessageBytes;
    const json::ParseResult parsed = json::parse(line, limits);
    if (!parsed.ok) {
        result.status = DecodeStatus::NotJson;
        // The parser's error never quotes payload content, so this is safe to log verbatim.
        result.error = parsed.error;
        return result;
    }
    if (!parsed.value.is_object()) {
        result.status = DecodeStatus::NotObject;
        result.error = "top-level value is not an object";
        return result;
    }

    const json::Value& doc = parsed.value;
    if (!doc.has("type")) {
        result.status = DecodeStatus::MissingField;
        result.error = "missing 'type'";
        return result;
    }

    Envelope envelope;
    envelope.version = static_cast<int>(doc.get_int("v", kProtocolVersion));
    // The version is checked before the type, because a peer speaking a protocol we do not know
    // may well be using type names that mean something else entirely.
    if (envelope.version < kProtocolMin || envelope.version > kProtocolMax) {
        result.status = DecodeStatus::BadVersion;
        result.error = "protocol version " + std::to_string(envelope.version) +
                       " is outside the supported range " + std::to_string(kProtocolMin) + "-" +
                       std::to_string(kProtocolMax);
        return result;
    }

    envelope.seq = doc.get_int("seq");
    envelope.ts = doc.get_int("ts");
    envelope.type = parse_message_type(doc.get_string("type"));
    if (envelope.type == MessageType::Unknown) {
        result.status = DecodeStatus::UnknownType;
        // Not fatal: a newer producer sending a type this build does not know is a message to
        // skip, not a reason to drop a working connection.
        result.error = "unrecognised message type";
        result.envelope = std::move(envelope);
        return result;
    }
    if (const json::Value* data = doc.find("data")) envelope.data = *data;

    result.status = DecodeStatus::Ok;
    result.envelope = std::move(envelope);
    return result;
}

json::Value HelloPayload::to_json() const {
    json::Object o;
    o.emplace_back("protocol_min", json::Value(protocol_min));
    o.emplace_back("protocol_max", json::Value(protocol_max));
    o.emplace_back("script_version", json::Value(script_version));
    o.emplace_back("obs_version", json::Value(obs_version));
    o.emplace_back("platform", json::Value(platform));
    json::Array caps;
    caps.reserve(capabilities.size());
    for (const std::string& c : capabilities) caps.emplace_back(c);
    o.emplace_back("capabilities", json::Value(std::move(caps)));
    return json::Value(std::move(o));
}

HelloPayload HelloPayload::from_json(const json::Value& v) {
    HelloPayload p;
    p.protocol_min = static_cast<int>(v.get_int("protocol_min", kProtocolMin));
    p.protocol_max = static_cast<int>(v.get_int("protocol_max", kProtocolMax));
    p.script_version = v.get_string("script_version");
    p.obs_version = v.get_string("obs_version");
    p.platform = v.get_string("platform");
    if (const json::Value* caps = v.find("capabilities"); caps != nullptr && caps->is_array()) {
        for (const json::Value& c : caps->as_array()) {
            if (c.is_string()) p.capabilities.push_back(c.as_string());
        }
    }
    return p;
}

json::Value ClientHelloPayload::to_json() const {
    json::Object o;
    o.emplace_back("protocol", json::Value(protocol));
    o.emplace_back("client", json::Value(client));
    o.emplace_back("client_version", json::Value(client_version));
    o.emplace_back("process", json::Value(process));
    o.emplace_back("pid", json::Value(pid));
    return json::Value(std::move(o));
}

ClientHelloPayload ClientHelloPayload::from_json(const json::Value& v) {
    ClientHelloPayload p;
    p.protocol = static_cast<int>(v.get_int("protocol", kProtocolVersion));
    p.client = v.get_string("client", "unknown");
    p.client_version = v.get_string("client_version");
    p.process = v.get_string("process");
    p.pid = v.get_int("pid");
    return p;
}

json::Value ErrorPayload::to_json() const {
    json::Object o;
    o.emplace_back("code", json::Value(code));
    o.emplace_back("message", json::Value(message));
    return json::Value(std::move(o));
}

ErrorPayload ErrorPayload::from_json(const json::Value& v) {
    ErrorPayload p;
    p.code = v.get_string("code");
    p.message = v.get_string("message");
    return p;
}

Envelope make(MessageType type, json::Value data, std::int64_t seq, std::int64_t ts) {
    Envelope e;
    e.type = type;
    e.data = std::move(data);
    e.seq = seq;
    e.ts = ts;
    return e;
}

void clamp_event(ObsEvent& event) {
    event.detail = json::truncate_utf8(event.detail, kMaxDetailChars);
    event.path = json::truncate_utf8(event.path, kMaxPathChars);
    event.scene = json::truncate_utf8(event.scene, kMaxDetailChars);
    event.previous_scene = json::truncate_utf8(event.previous_scene, kMaxDetailChars);
    event.profile = json::truncate_utf8(event.profile, kMaxDetailChars);
    event.service = json::truncate_utf8(event.service, kMaxDetailChars);
    // Negative durations and sizes are the "not known" sentinel; anything else negative is a
    // producer bug and is normalised to that same sentinel rather than formatted.
    if (event.duration_ms < 0) event.duration_ms = -1;
    if (event.size_bytes < 0) event.size_bytes = -1;
    if (event.attempt < 0) event.attempt = 0;
    if (event.replay_seconds < 0) event.replay_seconds = 0;
}

}  // namespace obsn::proto
