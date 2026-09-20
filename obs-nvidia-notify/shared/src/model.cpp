// SPDX-License-Identifier: MIT
#include "obsn/model.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace obsn {
namespace {

struct EnumName {
    const char* name;
    int value;
};

template <typename E, std::size_t N>
const char* name_of(const EnumName (&table)[N], E value, const char* fallback) noexcept {
    for (const EnumName& entry : table) {
        if (entry.value == static_cast<int>(value)) return entry.name;
    }
    return fallback;
}

template <typename E, std::size_t N>
bool value_of(const EnumName (&table)[N], std::string_view text, E& out) noexcept {
    for (const EnumName& entry : table) {
        if (text == entry.name) {
            out = static_cast<E>(entry.value);
            return true;
        }
    }
    return false;
}

constexpr EnumName kOutputStates[] = {
    {"idle", static_cast<int>(OutputState::Idle)},
    {"starting", static_cast<int>(OutputState::Starting)},
    {"active", static_cast<int>(OutputState::Active)},
    {"paused", static_cast<int>(OutputState::Paused)},
    {"stopping", static_cast<int>(OutputState::Stopping)},
    {"reconnecting", static_cast<int>(OutputState::Reconnecting)},
};

constexpr EnumName kStopReasons[] = {
    {"user", static_cast<int>(StopReason::User)},
    {"error", static_cast<int>(StopReason::Error)},
    {"out_of_space", static_cast<int>(StopReason::OutOfSpace)},
    {"encoder_error", static_cast<int>(StopReason::EncoderError)},
    {"obs_exiting", static_cast<int>(StopReason::ObsExiting)},
    {"unknown", static_cast<int>(StopReason::Unknown)},
};

// The wire names are the OBS frontend event names, lower-cased and dotted. Keeping them
// recognisable is deliberate: a pipe trace should be readable by someone who knows OBS and has
// never read this protocol.
constexpr EnumName kEventKinds[] = {
    {"unknown", static_cast<int>(EventKind::Unknown)},
    {"recording.starting", static_cast<int>(EventKind::RecordingStarting)},
    {"recording.started", static_cast<int>(EventKind::RecordingStarted)},
    {"recording.stopping", static_cast<int>(EventKind::RecordingStopping)},
    {"recording.stopped", static_cast<int>(EventKind::RecordingStopped)},
    {"recording.paused", static_cast<int>(EventKind::RecordingPaused)},
    {"recording.resumed", static_cast<int>(EventKind::RecordingResumed)},
    {"recording.saved", static_cast<int>(EventKind::RecordingSaved)},
    {"replay.starting", static_cast<int>(EventKind::ReplayBufferStarting)},
    {"replay.started", static_cast<int>(EventKind::ReplayBufferStarted)},
    {"replay.stopped", static_cast<int>(EventKind::ReplayBufferStopped)},
    {"replay.saved", static_cast<int>(EventKind::ReplayBufferSaved)},
    {"stream.starting", static_cast<int>(EventKind::StreamStarting)},
    {"stream.started", static_cast<int>(EventKind::StreamStarted)},
    {"stream.stopping", static_cast<int>(EventKind::StreamStopping)},
    {"stream.stopped", static_cast<int>(EventKind::StreamStopped)},
    {"stream.reconnecting", static_cast<int>(EventKind::StreamReconnecting)},
    {"stream.reconnected", static_cast<int>(EventKind::StreamReconnected)},
    {"virtualcam.started", static_cast<int>(EventKind::VirtualCamStarted)},
    {"virtualcam.stopped", static_cast<int>(EventKind::VirtualCamStopped)},
    {"scene.changed", static_cast<int>(EventKind::SceneChanged)},
    {"profile.changed", static_cast<int>(EventKind::ProfileChanged)},
    {"warning", static_cast<int>(EventKind::Warning)},
    {"script.connected", static_cast<int>(EventKind::ScriptConnected)},
    {"script.disconnected", static_cast<int>(EventKind::ScriptDisconnected)},
};

json::Value output_json(OutputState state, std::int64_t started_ms) {
    json::Object o;
    o.emplace_back("state", json::Value(to_string(state)));
    o.emplace_back("started_ms", json::Value(started_ms));
    return json::Value(std::move(o));
}

OutputState read_state(const json::Value& v) {
    OutputState state = OutputState::Idle;
    parse_enum(v.get_string("state"), state);
    return state;
}

}  // namespace

const char* to_string(OutputState v) noexcept { return name_of(kOutputStates, v, "idle"); }
bool parse_enum(std::string_view t, OutputState& out) noexcept {
    return value_of(kOutputStates, t, out);
}

const char* to_string(StopReason v) noexcept { return name_of(kStopReasons, v, "unknown"); }
bool parse_enum(std::string_view t, StopReason& out) noexcept {
    return value_of(kStopReasons, t, out);
}

const char* to_string(EventKind v) noexcept { return name_of(kEventKinds, v, "unknown"); }
bool parse_enum(std::string_view t, EventKind& out) noexcept {
    return value_of(kEventKinds, t, out);
}

OutputKind output_of(EventKind kind) noexcept {
    switch (kind) {
        case EventKind::RecordingStarting:
        case EventKind::RecordingStarted:
        case EventKind::RecordingStopping:
        case EventKind::RecordingStopped:
        case EventKind::RecordingPaused:
        case EventKind::RecordingResumed:
        case EventKind::RecordingSaved:
            return OutputKind::Recording;
        case EventKind::ReplayBufferStarting:
        case EventKind::ReplayBufferStarted:
        case EventKind::ReplayBufferStopped:
        case EventKind::ReplayBufferSaved:
            return OutputKind::ReplayBuffer;
        case EventKind::StreamStarting:
        case EventKind::StreamStarted:
        case EventKind::StreamStopping:
        case EventKind::StreamStopped:
        case EventKind::StreamReconnecting:
        case EventKind::StreamReconnected:
            return OutputKind::Stream;
        case EventKind::VirtualCamStarted:
        case EventKind::VirtualCamStopped:
            return OutputKind::VirtualCam;
        case EventKind::SceneChanged:
        case EventKind::ProfileChanged:
        case EventKind::Warning:
        case EventKind::ScriptConnected:
        case EventKind::ScriptDisconnected:
            return OutputKind::Session;
        case EventKind::Unknown:
            return OutputKind::None;
    }
    return OutputKind::None;
}

std::int64_t RecordingState::elapsed_ms(std::int64_t now_ms) const noexcept {
    if (!active() || started_ms <= 0) return 0;
    std::int64_t paused = paused_ms;
    // A recording paused right now is still accumulating paused time, so the visible timer
    // must hold still rather than continuing to climb until the user resumes.
    if (state == OutputState::Paused && paused_since_ms > 0) {
        paused += std::max<std::int64_t>(0, now_ms - paused_since_ms);
    }
    return std::max<std::int64_t>(0, now_ms - started_ms - paused);
}

std::int64_t StreamState::elapsed_ms(std::int64_t now_ms) const noexcept {
    if (started_ms <= 0 || state == OutputState::Idle) return 0;
    return std::max<std::int64_t>(0, now_ms - started_ms);
}

std::string file_name_of(std::string_view path) {
    const std::size_t cut = path.find_last_of("/\\");
    if (cut == std::string_view::npos) return std::string(path);
    return std::string(path.substr(cut + 1));
}

std::string format_duration(std::int64_t ms) {
    if (ms < 0) ms = 0;
    const std::int64_t total = ms / 1000;
    const std::int64_t hours = total / 3600;
    const std::int64_t minutes = (total % 3600) / 60;
    const std::int64_t seconds = total % 60;
    char buffer[32];
    int n;
    if (hours > 0) {
        n = std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld",
                          static_cast<long long>(hours), static_cast<long long>(minutes),
                          static_cast<long long>(seconds));
    } else {
        n = std::snprintf(buffer, sizeof(buffer), "%lld:%02lld",
                          static_cast<long long>(minutes), static_cast<long long>(seconds));
    }
    return std::string(buffer, static_cast<std::size_t>(n > 0 ? n : 0));
}

std::string format_size(std::int64_t bytes) {
    if (bytes < 0) return {};
    const char* unit = "B";
    double value = static_cast<double>(bytes);
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    std::size_t index = 0;
    while (value >= 1024.0 && index + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++index;
    }
    unit = units[index];
    char buffer[32];
    // One decimal place from megabytes upwards, none below: "980 KB" is precise enough, and
    // "1.4 GB" is what a person reads off a file listing.
    const int n = index >= 2 ? std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, unit)
                             : std::snprintf(buffer, sizeof(buffer), "%.0f %s", value, unit);
    return std::string(buffer, static_cast<std::size_t>(n > 0 ? n : 0));
}

json::Value to_json(const ObsState& s) {
    json::Object o;
    o.emplace_back("obs_version", json::Value(s.obs_version));
    o.emplace_back("script_version", json::Value(s.script_version));
    o.emplace_back("profile", json::Value(s.profile));
    o.emplace_back("scene_collection", json::Value(s.scene_collection));
    o.emplace_back("current_scene", json::Value(s.current_scene));

    {
        json::Value rec = output_json(s.recording.state, s.recording.started_ms);
        rec.set("paused_ms", json::Value(s.recording.paused_ms));
        rec.set("paused_since_ms", json::Value(s.recording.paused_since_ms));
        rec.set("path", json::Value(s.recording.path));
        o.emplace_back("recording", std::move(rec));
    }
    {
        json::Value rb = output_json(s.replay.state, s.replay.started_ms);
        rb.set("last_saved_path", json::Value(s.replay.last_saved_path));
        rb.set("last_saved_ms", json::Value(s.replay.last_saved_ms));
        rb.set("duration_s", json::Value(s.replay.duration_s));
        o.emplace_back("replay", std::move(rb));
    }
    {
        json::Value st = output_json(s.stream.state, s.stream.started_ms);
        st.set("service", json::Value(s.stream.service));
        st.set("reconnect_attempt", json::Value(s.stream.reconnect_attempt));
        o.emplace_back("stream", std::move(st));
    }
    o.emplace_back("virtual_cam", output_json(s.virtual_cam.state, s.virtual_cam.started_ms));

    // Stats are written only when present. An absent key is how "OBS did not say" is carried;
    // writing a zero would be indistinguishable from a real measurement of zero.
    json::Object stats;
    if (s.stats.have_fps) stats.emplace_back("fps", json::Value(s.stats.fps));
    if (s.stats.have_dropped) {
        stats.emplace_back("dropped_frames", json::Value(s.stats.dropped_frames));
        stats.emplace_back("dropped_percent", json::Value(s.stats.dropped_percent));
    }
    if (s.stats.have_skipped) stats.emplace_back("skipped_frames", json::Value(s.stats.skipped_frames));
    if (s.stats.have_disk) stats.emplace_back("free_disk_mb", json::Value(s.stats.free_disk_mb));
    if (s.stats.have_bitrate) stats.emplace_back("bitrate_kbps", json::Value(s.stats.bitrate_kbps));
    if (s.stats.have_cpu) stats.emplace_back("cpu_percent", json::Value(s.stats.cpu_percent));
    o.emplace_back("stats", json::Value(std::move(stats)));

    return json::Value(std::move(o));
}

ObsState state_from_json(const json::Value& v) {
    ObsState s;
    s.obs_running = true;
    s.obs_version = v.get_string("obs_version");
    s.script_version = v.get_string("script_version");
    s.profile = v.get_string("profile");
    s.scene_collection = v.get_string("scene_collection");
    s.current_scene = v.get_string("current_scene");

    if (const json::Value* rec = v.find("recording")) {
        s.recording.state = read_state(*rec);
        s.recording.started_ms = rec->get_int("started_ms");
        s.recording.paused_ms = rec->get_int("paused_ms");
        s.recording.paused_since_ms = rec->get_int("paused_since_ms");
        s.recording.path = rec->get_string("path");
    }
    if (const json::Value* rb = v.find("replay")) {
        s.replay.state = read_state(*rb);
        s.replay.started_ms = rb->get_int("started_ms");
        s.replay.last_saved_path = rb->get_string("last_saved_path");
        s.replay.last_saved_ms = rb->get_int("last_saved_ms");
        s.replay.duration_s = static_cast<int>(rb->get_int("duration_s"));
    }
    if (const json::Value* st = v.find("stream")) {
        s.stream.state = read_state(*st);
        s.stream.started_ms = st->get_int("started_ms");
        s.stream.service = st->get_string("service");
        s.stream.reconnect_attempt = static_cast<int>(st->get_int("reconnect_attempt"));
    }
    if (const json::Value* vc = v.find("virtual_cam")) {
        s.virtual_cam.state = read_state(*vc);
        s.virtual_cam.started_ms = vc->get_int("started_ms");
    }
    if (const json::Value* stats = v.find("stats")) {
        if (const json::Value* f = stats->find("fps")) {
            s.stats.have_fps = true;
            s.stats.fps = f->as_double();
        }
        if (const json::Value* d = stats->find("dropped_frames")) {
            s.stats.have_dropped = true;
            s.stats.dropped_frames = static_cast<int>(d->as_int());
            s.stats.dropped_percent = stats->get_double("dropped_percent");
        }
        if (const json::Value* k = stats->find("skipped_frames")) {
            s.stats.have_skipped = true;
            s.stats.skipped_frames = static_cast<int>(k->as_int());
        }
        if (const json::Value* disk = stats->find("free_disk_mb")) {
            s.stats.have_disk = true;
            s.stats.free_disk_mb = disk->as_int();
        }
        if (const json::Value* b = stats->find("bitrate_kbps")) {
            s.stats.have_bitrate = true;
            s.stats.bitrate_kbps = b->as_double();
        }
        if (const json::Value* c = stats->find("cpu_percent")) {
            s.stats.have_cpu = true;
            s.stats.cpu_percent = c->as_double();
        }
    }
    return s;
}

json::Value to_json(const ObsEvent& e) {
    json::Object o;
    o.emplace_back("kind", json::Value(to_string(e.kind)));
    o.emplace_back("ts", json::Value(e.ts));
    if (!e.path.empty()) o.emplace_back("path", json::Value(e.path));
    if (e.duration_ms >= 0) o.emplace_back("duration_ms", json::Value(e.duration_ms));
    if (e.size_bytes >= 0) o.emplace_back("size_bytes", json::Value(e.size_bytes));
    if (!e.scene.empty()) o.emplace_back("scene", json::Value(e.scene));
    if (!e.previous_scene.empty()) o.emplace_back("previous_scene", json::Value(e.previous_scene));
    if (!e.profile.empty()) o.emplace_back("profile", json::Value(e.profile));
    if (!e.service.empty()) o.emplace_back("service", json::Value(e.service));
    if (e.reason != StopReason::Unknown) o.emplace_back("reason", json::Value(to_string(e.reason)));
    if (!e.detail.empty()) o.emplace_back("detail", json::Value(e.detail));
    if (e.attempt > 0) o.emplace_back("attempt", json::Value(e.attempt));
    if (e.replay_seconds > 0) o.emplace_back("replay_seconds", json::Value(e.replay_seconds));
    if (e.carries_state) o.emplace_back("state", to_json(e.state));
    return json::Value(std::move(o));
}

ObsEvent event_from_json(const json::Value& v) {
    ObsEvent e;
    parse_enum(v.get_string("kind"), e.kind);
    e.ts = v.get_int("ts");
    e.path = v.get_string("path");
    e.duration_ms = v.has("duration_ms") ? v.get_int("duration_ms") : -1;
    e.size_bytes = v.has("size_bytes") ? v.get_int("size_bytes") : -1;
    e.scene = v.get_string("scene");
    e.previous_scene = v.get_string("previous_scene");
    e.profile = v.get_string("profile");
    e.service = v.get_string("service");
    parse_enum(v.get_string("reason"), e.reason);
    e.detail = v.get_string("detail");
    e.attempt = static_cast<int>(v.get_int("attempt"));
    e.replay_seconds = static_cast<int>(v.get_int("replay_seconds"));
    if (const json::Value* state = v.find("state"); state != nullptr && state->is_object()) {
        e.carries_state = true;
        e.state = state_from_json(*state);
    }
    return e;
}

}  // namespace obsn
