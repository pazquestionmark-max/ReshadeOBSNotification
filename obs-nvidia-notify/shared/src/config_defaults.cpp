// SPDX-License-Identifier: MIT
// The shipped look, and the category table everything else is built from.
//
// These defaults reproduce the NVIDIA overlay's capture notifications: a near-black panel with
// a square accent tile on the leading edge, a medium-weight title over a grey detail line, and
// a slide-in from the anchored screen edge that settles rather than bounces. Wording follows
// the same house style -- a short verb phrase, sentence case, no trailing full stop.
//
// The figures were matched against the reference by eye and by frame-stepping capture footage;
// docs/nvidia-reference.md records which are measured and which are judged, because "the same
// to a T" is a claim that should be checkable rather than asserted.
#include "obsn/config.hpp"

namespace obsn {
namespace {

/// Every category shares one silhouette. Only the wording, the glyph and the accent differ,
/// which is exactly what makes a set of notifications read as one system.
NotificationStyle base(std::string title, std::string detail, IconShape icon, Color accent) {
    NotificationStyle s;
    s.title_format = std::move(title);
    s.detail_format = std::move(detail);
    s.icon = icon;
    s.accent = accent;
    return s;
}

// The accent family. Every one is the house green pushed towards a hue that matches what the
// event means, so the set stays related instead of looking like six unrelated products.
constexpr Color kGreen = kNvidiaGreen;
constexpr Color kRed{235, 64, 52, 255};       ///< recording: a record light is red everywhere
constexpr Color kAmber{240, 180, 40, 255};    ///< paused, reconnecting -- attention, not alarm
constexpr Color kBlue{80, 160, 235, 255};     ///< streaming and the virtual camera
constexpr Color kGrey{140, 146, 152, 255};    ///< things that stopped cleanly, and scene changes

}  // namespace

Config Config::defaults() {
    Config c;
    NotificationsConfig& n = c.notifications;

    // --- recording -------------------------------------------------------------------------
    n.recording_started = base("Recording started", "", IconShape::Record, kRed);
    n.recording_stopped = base("Recording stopped", "{duration}", IconShape::Stop, kGrey);
    n.recording_paused = base("Recording paused", "", IconShape::Pause, kAmber);
    n.recording_resumed = base("Recording resumed", "", IconShape::Record, kRed);
    // The one event where the detail line is the point: a saved file the user will go looking
    // for, named so they can find it.
    n.recording_saved = base("Recording saved", "{file}", IconShape::Save, kGreen);
    n.recording_saved.wrap = true;

    // --- replay buffer ---------------------------------------------------------------------
    // The reference phrases the buffer as a capability being switched on, not an output that
    // started, and that distinction is worth keeping: nothing is being written yet.
    n.replay_started = base("Replay buffer is on", "", IconShape::Replay, kGreen);
    n.replay_stopped = base("Replay buffer is off", "", IconShape::Replay, kGrey);
    n.replay_saved = base("Replay saved", "{file}", IconShape::Save, kGreen);
    n.replay_saved.wrap = true;
    // A replay is saved in the middle of play, when the user is least able to read: it gets a
    // longer hold than anything else so it is still there when they look up.
    n.replay_saved.fade.hold_ms = 4000;
    n.replay_saved.priority = 2;

    // --- streaming -------------------------------------------------------------------------
    n.stream_started = base("Stream started", "{service}", IconShape::Broadcast, kBlue);
    n.stream_stopped = base("Stream stopped", "{duration}", IconShape::Stop, kGrey);
    n.stream_reconnecting = base("Reconnecting", "Attempt {attempt}", IconShape::Warning, kAmber);
    n.stream_reconnecting.priority = 3;
    n.stream_reconnected = base("Reconnected", "", IconShape::Check, kGreen);
    n.stream_reconnected.priority = 3;

    // --- virtual camera ----------------------------------------------------------------------
    n.virtual_cam_started = base("Virtual camera on", "", IconShape::Camera, kBlue);
    n.virtual_cam_stopped = base("Virtual camera off", "", IconShape::Camera, kGrey);

    // --- session ---------------------------------------------------------------------------
    // Off by default, all three. A scene change is something the user just did themselves, and
    // the two link toasts are about the overlay's own plumbing rather than about OBS.
    n.scene_changed = base("Scene changed", "{scene}", IconShape::Layers, kGrey);
    n.scene_changed.enabled = false;
    n.obs_connected = base("OBS connected", "", IconShape::Check, kGreen);
    n.obs_connected.enabled = false;
    n.obs_disconnected = base("OBS disconnected", "", IconShape::Cross, kGrey);
    n.obs_disconnected.enabled = false;

    // A warning is the one category that is allowed to interrupt: it outlives everything in the
    // queue, holds longest, and says what OBS said rather than a paraphrase of it.
    n.warning = base("{detail}", "", IconShape::Warning, kAmber);
    n.warning.fade.hold_ms = 5000;
    n.warning.wrap = true;
    n.warning.max_lines = 3;
    n.warning.priority = 5;

    return c;
}

const std::vector<CategoryEntry>& categories() {
    // Ordered as the settings UI lists them: by output, then by the order events occur within
    // that output, which is the order someone looking for one will scan in.
    static const std::vector<CategoryEntry> table = {
        {"recording_started", "Recording started",
         &NotificationsConfig::recording_started, EventKind::RecordingStarted},
        {"recording_stopped", "Recording stopped",
         &NotificationsConfig::recording_stopped, EventKind::RecordingStopped},
        {"recording_paused", "Recording paused",
         &NotificationsConfig::recording_paused, EventKind::RecordingPaused},
        {"recording_resumed", "Recording resumed",
         &NotificationsConfig::recording_resumed, EventKind::RecordingResumed},
        {"recording_saved", "Recording saved",
         &NotificationsConfig::recording_saved, EventKind::RecordingSaved},
        {"replay_started", "Replay buffer on",
         &NotificationsConfig::replay_started, EventKind::ReplayBufferStarted},
        {"replay_stopped", "Replay buffer off",
         &NotificationsConfig::replay_stopped, EventKind::ReplayBufferStopped},
        {"replay_saved", "Replay saved",
         &NotificationsConfig::replay_saved, EventKind::ReplayBufferSaved},
        {"stream_started", "Stream started",
         &NotificationsConfig::stream_started, EventKind::StreamStarted},
        {"stream_stopped", "Stream stopped",
         &NotificationsConfig::stream_stopped, EventKind::StreamStopped},
        {"stream_reconnecting", "Stream reconnecting",
         &NotificationsConfig::stream_reconnecting, EventKind::StreamReconnecting},
        {"stream_reconnected", "Stream reconnected",
         &NotificationsConfig::stream_reconnected, EventKind::StreamReconnected},
        {"virtual_cam_started", "Virtual camera on",
         &NotificationsConfig::virtual_cam_started, EventKind::VirtualCamStarted},
        {"virtual_cam_stopped", "Virtual camera off",
         &NotificationsConfig::virtual_cam_stopped, EventKind::VirtualCamStopped},
        {"scene_changed", "Scene changed",
         &NotificationsConfig::scene_changed, EventKind::SceneChanged},
        {"warning", "Warning", &NotificationsConfig::warning, EventKind::Warning},
        {"obs_connected", "OBS connected",
         &NotificationsConfig::obs_connected, EventKind::ScriptConnected},
        {"obs_disconnected", "OBS disconnected",
         &NotificationsConfig::obs_disconnected, EventKind::ScriptDisconnected},
    };
    return table;
}

const NotificationStyle* Config::style_for(EventKind kind) const noexcept {
    return const_cast<Config*>(this)->style_for(kind);
}

NotificationStyle* Config::style_for(EventKind kind) noexcept {
    for (const CategoryEntry& entry : categories()) {
        if (entry.kind == kind) return &(notifications.*(entry.member));
    }
    // Deliberately not a fallback to some default category. A kind with no style is one that
    // never raises a toast -- the "starting"/"stopping" transitions, which exist in the model
    // so the status indicator can show them, and would only be noise as notifications.
    return nullptr;
}

}  // namespace obsn
