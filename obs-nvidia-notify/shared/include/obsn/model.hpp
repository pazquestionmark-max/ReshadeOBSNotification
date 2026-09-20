// SPDX-License-Identifier: MIT
// What OBS is doing, and what just happened.
//
// Two distinct shapes, kept apart on purpose:
//   * ObsState is a *level* -- what is true right now. It is what a status indicator draws and
//     what a freshly-connected overlay is handed so it does not have to infer the world from a
//     history it missed.
//   * ObsEvent is an *edge* -- something that happened at an instant. Only edges become
//     notifications, which is why "recording is active" never raises a toast but "recording
//     started" does.
//
// Nothing here knows about OBS's API, ReShade, or ImGui: this is the vocabulary the two ends
// share, and it is why the whole pipeline is testable on a machine with neither installed.
#ifndef OBSN_MODEL_HPP
#define OBSN_MODEL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "obsn/json.hpp"

namespace obsn {

/// The lifecycle every OBS output shares. OBS reports starting and stopping as their own
/// frontend events, and they are kept distinct rather than collapsed into a boolean: a stream
/// that is still negotiating is not a stream that is live, and showing it as live is the kind
/// of confident wrong answer this project does not render.
enum class OutputState {
    Idle,
    Starting,
    Active,
    Paused,        ///< recording only; OBS has no paused stream
    Stopping,
    Reconnecting,  ///< streaming only; the connection dropped and OBS is retrying
};

const char* to_string(OutputState) noexcept;
bool parse_enum(std::string_view, OutputState&) noexcept;

struct RecordingState {
    OutputState state = OutputState::Idle;
    /// When the current recording began, in wall-clock milliseconds. 0 when idle.
    std::int64_t started_ms = 0;
    /// Total time spent paused within the current recording, so elapsed time can exclude it.
    std::int64_t paused_ms = 0;
    /// When the current pause began; 0 when not paused. Kept separate from `paused_ms` so the
    /// timer can tick correctly *during* a pause rather than only after it ends.
    std::int64_t paused_since_ms = 0;
    std::string path;          ///< the file being written, when OBS will tell us
    bool splitting = false;    ///< a file split is in progress (OBS 30+ automatic splitting)

    bool active() const noexcept {
        return state == OutputState::Active || state == OutputState::Paused;
    }
    /// Recorded duration at `now_ms`, excluding paused time. 0 when not recording.
    std::int64_t elapsed_ms(std::int64_t now_ms) const noexcept;
};

struct ReplayBufferState {
    OutputState state = OutputState::Idle;
    std::int64_t started_ms = 0;
    std::string last_saved_path;
    std::int64_t last_saved_ms = 0;
    /// How much footage the buffer holds, from OBS's own setting. 0 when unknown -- absent is
    /// not zero, and the overlay omits the figure rather than claiming a 0-second buffer.
    int duration_s = 0;

    bool active() const noexcept { return state == OutputState::Active; }
};

struct StreamState {
    OutputState state = OutputState::Idle;
    std::int64_t started_ms = 0;
    std::string service;       ///< "Twitch", "YouTube - RTMPS", ... as OBS names it
    int reconnect_attempt = 0;

    bool active() const noexcept { return state == OutputState::Active; }
    std::int64_t elapsed_ms(std::int64_t now_ms) const noexcept;
};

struct VirtualCamState {
    OutputState state = OutputState::Idle;
    std::int64_t started_ms = 0;
    bool active() const noexcept { return state == OutputState::Active; }
};

/// Health figures OBS publishes continuously. Every one is optional, because OBS does not
/// always have an answer and a fabricated zero reads as a real measurement.
struct OutputStats {
    bool have_fps = false;
    double fps = 0.0;
    bool have_dropped = false;
    int dropped_frames = 0;
    double dropped_percent = 0.0;
    bool have_skipped = false;
    int skipped_frames = 0;       ///< encoder overload, distinct from network drops
    bool have_disk = false;
    std::int64_t free_disk_mb = 0;
    bool have_bitrate = false;
    double bitrate_kbps = 0.0;
    bool have_cpu = false;
    double cpu_percent = 0.0;
};

/// Everything the overlay knows about the OBS session.
struct ObsState {
    bool obs_running = false;      ///< the script is connected and talking to us
    std::string obs_version;
    std::string script_version;
    std::string profile;
    std::string scene_collection;
    std::string current_scene;
    RecordingState recording;
    ReplayBufferState replay;
    StreamState stream;
    VirtualCamState virtual_cam;
    OutputStats stats;
    std::int64_t updated_ms = 0;

    /// True when any output is running, which is what the status indicator keys off.
    bool any_output_active() const noexcept {
        return recording.active() || replay.active() || stream.active() || virtual_cam.active();
    }
};

/// Why a recording or buffer stopped. OBS distinguishes these and so does the overlay: a
/// recording that ran out of disk is not a recording the user stopped, and telling them
/// otherwise loses the one piece of information that mattered.
enum class StopReason {
    User,
    Error,
    OutOfSpace,
    EncoderError,
    ObsExiting,
    Unknown,
};

const char* to_string(StopReason) noexcept;
bool parse_enum(std::string_view, StopReason&) noexcept;

/// Something that happened, at an instant. These are the only things that become toasts.
enum class EventKind {
    Unknown,
    RecordingStarting,
    RecordingStarted,
    RecordingStopping,
    RecordingStopped,
    RecordingPaused,
    RecordingResumed,
    /// The file finished being written. Distinct from RecordingStopped because OBS emits the
    /// path separately, and because an automatic split saves a file without stopping anything.
    RecordingSaved,
    ReplayBufferStarting,
    ReplayBufferStarted,
    ReplayBufferStopped,
    ReplayBufferSaved,
    StreamStarting,
    StreamStarted,
    StreamStopping,
    StreamStopped,
    StreamReconnecting,
    StreamReconnected,
    VirtualCamStarted,
    VirtualCamStopped,
    SceneChanged,
    ProfileChanged,
    /// A condition the user should know about while it is still fixable: disk filling,
    /// frames dropping, encoder overloaded.
    Warning,
    /// The link to OBS itself came up or went down. Never a toast by default -- the overlay
    /// simply stops claiming to know what OBS is doing.
    ScriptConnected,
    ScriptDisconnected,
};

const char* to_string(EventKind) noexcept;
bool parse_enum(std::string_view, EventKind&) noexcept;

/// Which output an event belongs to, so a category can be enabled or silenced as a group.
enum class OutputKind { None, Recording, ReplayBuffer, Stream, VirtualCam, Session };
OutputKind output_of(EventKind) noexcept;

struct ObsEvent {
    EventKind kind = EventKind::Unknown;
    std::int64_t ts = 0;

    /// The file this event concerns, as OBS reported it. Full path; the formatter decides
    /// whether to show the whole thing or just the name.
    std::string path;
    /// Length of the recording or clip, when known. -1 when it is not.
    std::int64_t duration_ms = -1;
    /// Size of the written file in bytes, when known. -1 when it is not.
    std::int64_t size_bytes = -1;

    std::string scene;
    std::string previous_scene;
    std::string profile;
    std::string service;

    StopReason reason = StopReason::Unknown;
    /// Free text for a Warning, and for an error OBS described in words we should not
    /// paraphrase. Never interpreted, only displayed.
    std::string detail;
    int attempt = 0;             ///< reconnect attempt number
    int replay_seconds = 0;      ///< length of a saved replay clip

    /// A complete state, sent with a snapshot rather than an edge. Only `ScriptConnected`
    /// carries one.
    bool carries_state = false;
    ObsState state;
};

/// The file name alone, with no directory. Handles both separators, because the path comes
/// from Windows OBS but the formatting code is also exercised on POSIX in CI.
std::string file_name_of(std::string_view path);

/// "1:23:45", or "4:07" under an hour. Used for recording length and elapsed time.
std::string format_duration(std::int64_t ms);

/// "1.4 GB", "812 MB", "44.2 KB". Sized so a recording's size reads at a glance.
std::string format_size(std::int64_t bytes);

json::Value to_json(const ObsState&);
ObsState state_from_json(const json::Value&);
json::Value to_json(const ObsEvent&);
ObsEvent event_from_json(const json::Value&);

}  // namespace obsn

#endif  // OBSN_MODEL_HPP
