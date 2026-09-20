// SPDX-License-Identifier: MIT
#include "obsn/model.hpp"
#include "obsn_test.hpp"

using namespace obsn;

TEST(model, duration_formatting) {
    CHECK_EQ(format_duration(0), std::string("0:00"));
    CHECK_EQ(format_duration(59'999), std::string("0:59"));
    CHECK_EQ(format_duration(60'000), std::string("1:00"));
    CHECK_EQ(format_duration(12 * 60'000 + 7'000), std::string("12:07"));
    // The hour boundary is where the format changes; 59:59 must not become 0:59:59.
    CHECK_EQ(format_duration(59 * 60'000 + 59'000), std::string("59:59"));
    CHECK_EQ(format_duration(3'600'000), std::string("1:00:00"));
    CHECK_EQ(format_duration(3 * 3'600'000 + 4 * 60'000 + 5'000), std::string("3:04:05"));
    // A negative is a caller bug, not a reason to print a negative clock.
    CHECK_EQ(format_duration(-5'000), std::string("0:00"));
}

TEST(model, size_formatting) {
    CHECK_EQ(format_size(0), std::string("0 B"));
    CHECK_EQ(format_size(999), std::string("999 B"));
    CHECK_EQ(format_size(1024), std::string("1 KB"));
    // One decimal from megabytes upwards, consistently: "1.0 MB" beside "1.4 GB".
    CHECK_EQ(format_size(1024 * 1024), std::string("1.0 MB"));
    CHECK_EQ(format_size(1536 * 1024), std::string("1.5 MB"));
    CHECK_EQ(format_size(1503238553), std::string("1.4 GB"));
    // -1 is the "not known" sentinel and must produce nothing at all, so the placeholder
    // collapses instead of printing a size that was never measured.
    CHECK_EQ(format_size(-1), std::string(""));
}

TEST(model, file_name_handles_both_separators) {
    CHECK_EQ(file_name_of("C:\\Users\\You\\Videos\\clip.mkv"), std::string("clip.mkv"));
    CHECK_EQ(file_name_of("/home/you/Videos/clip.mkv"), std::string("clip.mkv"));
    CHECK_EQ(file_name_of("clip.mkv"), std::string("clip.mkv"));
    CHECK_EQ(file_name_of(""), std::string(""));
}

TEST(model, elapsed_excludes_paused_time) {
    RecordingState r;
    r.state = OutputState::Active;
    r.started_ms = 1'000;
    r.paused_ms = 2'000;
    CHECK_EQ(r.elapsed_ms(11'000), 8'000);

    // While paused, the timer must hold still rather than continuing to climb.
    r.state = OutputState::Paused;
    r.paused_since_ms = 6'000;
    CHECK_EQ(r.elapsed_ms(11'000), 3'000);
    CHECK_EQ(r.elapsed_ms(20'000), 3'000);

    RecordingState idle;
    CHECK_EQ(idle.elapsed_ms(11'000), 0);
}

TEST(model, event_round_trips_through_json) {
    ObsEvent event;
    event.kind = EventKind::ReplayBufferSaved;
    event.ts = 1'700'000'000'000;
    event.path = "C:\\clips\\Replay.mkv";
    event.duration_ms = 30'000;
    event.size_bytes = 91'234'567;
    event.replay_seconds = 30;
    event.reason = StopReason::User;

    const ObsEvent back = event_from_json(to_json(event));
    CHECK(back.kind == EventKind::ReplayBufferSaved);
    CHECK_EQ(back.path, event.path);
    CHECK_EQ(back.duration_ms, event.duration_ms);
    CHECK_EQ(back.size_bytes, event.size_bytes);
    CHECK_EQ(back.replay_seconds, 30);
    CHECK(back.reason == StopReason::User);
}

TEST(model, absent_duration_survives_the_round_trip_as_absent) {
    // The distinction between "no duration" and "zero duration" is the whole reason these are
    // -1 rather than 0, so it has to survive serialisation.
    ObsEvent event;
    event.kind = EventKind::RecordingStarted;
    CHECK_EQ(event.duration_ms, -1);
    const ObsEvent back = event_from_json(to_json(event));
    CHECK_EQ(back.duration_ms, -1);
    CHECK_EQ(back.size_bytes, -1);
}

TEST(model, state_round_trips_and_omits_absent_stats) {
    ObsState state;
    state.obs_version = "30.2.3";
    state.current_scene = "Gameplay";
    state.recording.state = OutputState::Paused;
    state.recording.started_ms = 500;
    state.recording.paused_ms = 100;
    state.replay.state = OutputState::Active;
    state.replay.duration_s = 30;
    state.stream.service = "Twitch";
    state.stats.have_fps = true;
    state.stats.fps = 59.94;

    const json::Value encoded = to_json(state);
    const json::Value* stats = encoded.find("stats");
    CHECK(stats != nullptr);
    CHECK(stats->has("fps"));
    // Nothing said anything about dropped frames, so the key must be absent rather than zero.
    CHECK(!stats->has("dropped_frames"));

    const ObsState back = state_from_json(encoded);
    CHECK(back.recording.state == OutputState::Paused);
    CHECK_EQ(back.recording.paused_ms, 100);
    CHECK(back.replay.state == OutputState::Active);
    CHECK_EQ(back.replay.duration_s, 30);
    CHECK_EQ(back.stream.service, std::string("Twitch"));
    CHECK(back.stats.have_fps);
    CHECK(!back.stats.have_dropped);
}

TEST(model, every_event_kind_has_a_stable_wire_name) {
    // A kind whose name does not round-trip is a kind the OBS script can never send.
    for (int i = 0; i <= static_cast<int>(EventKind::ScriptDisconnected); ++i) {
        const EventKind kind = static_cast<EventKind>(i);
        EventKind parsed = EventKind::Unknown;
        CHECK(parse_enum(to_string(kind), parsed));
        CHECK(parsed == kind);
    }
}

TEST(model, events_are_grouped_by_the_output_they_belong_to) {
    CHECK(output_of(EventKind::RecordingSaved) == OutputKind::Recording);
    CHECK(output_of(EventKind::ReplayBufferSaved) == OutputKind::ReplayBuffer);
    CHECK(output_of(EventKind::StreamReconnecting) == OutputKind::Stream);
    CHECK(output_of(EventKind::VirtualCamStarted) == OutputKind::VirtualCam);
    CHECK(output_of(EventKind::Warning) == OutputKind::Session);
}
