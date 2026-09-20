// SPDX-License-Identifier: MIT
// Versioned, self-repairing overlay configuration.
//
// Two invariants hold throughout:
//   * Loading never fails and never throws. Bad values are clamped or defaulted and every
//     repair is recorded in ConfigDiagnostics so the user can see what happened.
//   * Unknown keys are preserved verbatim, so a config written by a newer build and opened by
//     an older one is not silently stripped of the settings it did not understand.
//
// The shipped defaults reproduce the NVIDIA overlay's notification look and motion. Every one
// of them is a setting, not a constant: the reference styling is a starting point the user can
// take apart entirely. See docs/nvidia-reference.md for what each default is reproducing and
// how faithful it claims to be.
#ifndef OBSN_CONFIG_HPP
#define OBSN_CONFIG_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "obsn/json.hpp"
#include "obsn/model.hpp"

namespace obsn {

inline constexpr int kConfigVersion = 1;

// --- primitives --------------------------------------------------------------------------

struct Color {
    std::uint8_t r = 255, g = 255, b = 255, a = 255;

    constexpr Color() = default;
    constexpr Color(std::uint8_t rr, std::uint8_t gg, std::uint8_t bb, std::uint8_t aa = 255)
        : r(rr), g(gg), b(bb), a(aa) {}

    /// Accepts #RGB, #RGBA, #RRGGBB, #RRGGBBAA, with or without the leading '#'.
    static std::optional<Color> from_hex(std::string_view);
    std::string to_hex() const;  ///< always "#RRGGBBAA"

    /// 0xAABBGGRR, the packed layout ImGui's draw list expects.
    std::uint32_t to_abgr() const noexcept {
        return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(b) << 16) |
               (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(r);
    }
    Color with_alpha_scale(float s) const noexcept;
    /// Linear blend towards `other`. Used for the accent tints, so one accent colour produces a
    /// whole family of related shades instead of six settings that can drift apart.
    Color mix(const Color& other, float t) const noexcept;
    bool operator==(const Color& o) const noexcept {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
    bool operator!=(const Color& o) const noexcept { return !(*this == o); }
};

/// NVIDIA's accent green, which every shipped category tints from. Named rather than repeated
/// so changing the house colour is one edit here and one setting in the UI.
inline constexpr Color kNvidiaGreen{118, 185, 0, 255};

enum class Anchor {
    TopLeft, TopCenter, TopRight,
    CenterLeft, Center, CenterRight,
    BottomLeft, BottomCenter, BottomRight,
};

enum class Align { Left, Center, Right };

/// Every glyph the renderer can draw. All are vector shapes built from ImDrawList primitives --
/// no image asset is shipped, and in particular no third party's logo or artwork is reproduced.
enum class IconShape {
    None, Dot, Circle, Ring, Square, Diamond, Triangle, Star, Chevron,
    Record,        ///< filled circle, the universal recording mark
    RecordRing,    ///< circle inside a ring, for "armed but not writing"
    Stop,          ///< filled square
    Pause,         ///< two vertical bars
    Play,
    Save,          ///< downward arrow into a tray
    Replay,        ///< circular arrow, anticlockwise
    Clock,
    Camera,
    Broadcast,     ///< a dot with two arcs, for streaming
    Film,
    Scissors,
    Warning,       ///< triangle with a bar
    Check,
    Cross,
    Folder,
    Layers,        ///< stacked sheets, for a scene change
    Disk,
};

enum class Easing {
    Linear, EaseIn, EaseOut, EaseInOut,
    EaseInCubic, EaseOutCubic, EaseInOutCubic,
    EaseOutQuint,   ///< the long, decisive settle the reference slide uses
    EaseOutExpo,
    EaseOutBack, EaseOutElastic,
};

enum class OverflowMode { Clip, Ellipsis, Wrap, Shrink, Scroll };

enum class StackDirection { Down, Up };

/// How a notification box's edge is drawn.
enum class NotificationBorder {
    None,
    Accent,   ///< the category's own colour, dimmed -- each toast is outlined in its own hue
    Custom,   ///< one colour for every category
};

/// Where the category colour shows on the box. The reference puts it in a tile behind the icon;
/// a bar down the leading edge is the other common treatment, and both can be on at once.
enum class AccentStyle { None, Bar, Tile, BarAndTile };

/// How a toast enters and leaves, beyond the opacity ramp.
///
/// SlideFromEdge is the reference behaviour: the box travels in from whichever screen edge it is
/// anchored to. It is a separate axis from the fade so the two can be tuned independently -- a
/// slide with no fade, or a fade with no slide, are both one setting away.
enum class MotionKind {
    None,
    SlideFromEdge,   ///< travels in from the anchored edge, along the axis that edge implies
    SlideHorizontal, ///< always along X, regardless of anchor
    SlideVertical,   ///< always along Y
    Scale,           ///< grows from `scale_from` about its own centre
};

const char* to_string(Anchor) noexcept;
const char* to_string(Align) noexcept;
const char* to_string(IconShape) noexcept;
const char* to_string(Easing) noexcept;
const char* to_string(OverflowMode) noexcept;
const char* to_string(StackDirection) noexcept;
const char* to_string(NotificationBorder) noexcept;
const char* to_string(AccentStyle) noexcept;
const char* to_string(MotionKind) noexcept;

bool parse_enum(std::string_view, Anchor&) noexcept;
bool parse_enum(std::string_view, Align&) noexcept;
bool parse_enum(std::string_view, IconShape&) noexcept;
bool parse_enum(std::string_view, Easing&) noexcept;
bool parse_enum(std::string_view, OverflowMode&) noexcept;
bool parse_enum(std::string_view, StackDirection&) noexcept;
bool parse_enum(std::string_view, NotificationBorder&) noexcept;
bool parse_enum(std::string_view, AccentStyle&) noexcept;
bool parse_enum(std::string_view, MotionKind&) noexcept;

/// Applies `easing` to t in [0,1]. Clamped, so a caller that overshoots gets the endpoint
/// rather than an extrapolation.
float ease(Easing, float t) noexcept;

/// Where an element sits. Offsets are pixels, or fractions of the viewport when `percent`.
struct Placement {
    bool visible = true;
    Anchor anchor = Anchor::TopRight;
    float x = 32.0f;
    float y = 32.0f;
    bool percent = false;
    Align align = Align::Right;
};

/// The opacity envelope: in, hold, out.
struct Fade {
    int in_ms = 260;
    int out_ms = 220;
    int hold_ms = 3200;   ///< fully-visible time between the two animations
    float start_opacity = 0.0f;
    float end_opacity = 1.0f;
    Easing easing = Easing::EaseOutCubic;
};

/// The positional envelope, running alongside the fade over the same in/out windows.
struct Motion {
    MotionKind kind = MotionKind::SlideFromEdge;
    /// Travel distance in unscaled pixels. 0 means "the box's own extent along the axis", which
    /// is what makes the toast start fully outside the screen edge as the reference does.
    float distance = 0.0f;
    /// Starting scale for MotionKind::Scale.
    float scale_from = 0.92f;
    Easing in_easing = Easing::EaseOutQuint;
    Easing out_easing = Easing::EaseInCubic;
    /// Slide back out the way it came. Off leaves the box in place and only fades it.
    bool exit_slides = true;
};

// --- sections ------------------------------------------------------------------------------

struct GeneralConfig {
    bool enabled = true;
    /// Draw anything at all while the OBS script is not connected. The status indicator honours
    /// this too: with OBS closed there is nothing true to say, so by default nothing is said.
    bool show_when_obs_closed = false;
    float master_opacity = 1.0f;
    float scale = 1.0f;
    std::string profile_name = "default";
    bool auto_profile_by_executable = true;
    /// Show every setting. Off by default: the common ones fit on two tabs.
    bool advanced_settings = false;
};

struct AppearanceConfig {
    /// The typeface the overlay draws with, as a file name inside its `fonts` folder.
    /// Empty means ReShade's own font.
    ///
    /// The add-on rasterises this itself rather than asking ReShade for it: ReShade owns the
    /// ImGui font atlas, and reading that atlas from an add-on is not safe across builds.
    /// Loading the .ttf here touches none of ReShade's ImGui state, so the font applies to the
    /// overlay alone and leaves ReShade's own UI untouched.
    std::string font_file = "Inter-Regular.ttf";
    int font_face_index = 0;
    /// Stroke weight on the usual 100-900 scale, emboldened at rasterisation time when the face
    /// ships only one weight. The reference title reads as a medium, not a bold.
    int title_weight = 600;
    int detail_weight = 400;
    float title_size = 15.0f;
    float detail_size = 13.0f;
    /// Gap between the title and the detail line.
    float line_gap = 3.0f;
    float icon_size = 20.0f;
    Color title_color{255, 255, 255, 255};
    /// The reference's secondary line is a mid grey, clearly subordinate without being unreadable.
    Color detail_color{168, 172, 176, 255};
    /// The house accent. Categories that do not override it inherit this.
    Color accent = kNvidiaGreen;
    bool text_shadow = true;          ///< keeps text legible over bright game content
    Color text_shadow_color{0, 0, 0, 170};
    float text_shadow_offset = 1.0f;
    /// A full outline rather than a one-sided shadow. Costs eight extra text draws per string,
    /// so it is off by default, but it is the only thing that stays readable over *any*
    /// background rather than most of them.
    bool text_outline = false;
    Color text_outline_color{0, 0, 0, 230};
    float text_outline_thickness = 1.0f;
};

/// The box every notification is drawn in.
///
/// Separate from the per-category colours so the *shape* of a toast -- how dark it is, how sharp
/// its corners are, whether it carries an edge -- is one setting rather than fifteen.
struct NotificationBoxStyle {
    /// Near-black and nearly opaque, as the reference is: a toast that shows the game through
    /// it is a toast you have to stop and read twice.
    Color background{17, 17, 17, 242};
    /// A second, slightly lighter colour blended down the box. 0 alpha disables the gradient,
    /// which is the honest default -- the reference panel is flat.
    Color background_gradient{0, 0, 0, 0};
    /// 0 is a hard rectangle. The reference is very nearly square.
    float corner_radius = 2.0f;
    NotificationBorder border = NotificationBorder::None;
    Color border_color{255, 255, 255, 28};
    float border_thickness = 1.0f;
    /// How strongly the category colour shows in the edge when `border` is Accent.
    float border_accent_opacity = 0.55f;

    AccentStyle accent_style = AccentStyle::Tile;
    /// Width of the vertical stripe down the leading edge, when `accent_style` uses a bar.
    float accent_bar_width = 3.0f;
    /// Width of the square region the icon sits in, when `accent_style` uses a tile. The tile is
    /// the full height of the box, which is what makes the reference toast read as two panels.
    float accent_tile_width = 52.0f;
    /// How the tile is filled: the category colour at this opacity behind the icon.
    float accent_tile_opacity = 1.0f;
    /// Draw the icon in the tile's contrasting colour rather than the accent. On a filled tile
    /// an accent-coloured icon is invisible, which is why this follows the tile automatically.
    Color tile_icon_color{12, 12, 12, 255};

    /// A drop shadow under the box. The reference sits on top of game content with a soft edge
    /// beneath it; without one the panel looks pasted on over bright scenes.
    bool shadow = true;
    Color shadow_color{0, 0, 0, 90};
    float shadow_size = 8.0f;
    float shadow_offset_y = 2.0f;

    /// Size each toast to its own text instead of a fixed column.
    bool auto_width = true;
    /// The cap on a toast that sizes itself.
    float max_width = 560.0f;
};

/// One notification category's look and wording.
struct NotificationStyle {
    bool enabled = true;
    /// The bold first line. Placeholders are expanded; see docs/configuration.md.
    std::string title_format = "Recording started";
    /// The subordinate second line. Empty draws a single-line toast, which is what the
    /// reference does for events with nothing further to say.
    std::string detail_format;
    IconShape icon = IconShape::Record;
    /// This category's colour, used for the tile, the bar and the accent border. Drives the
    /// whole toast's identity, which is why it is one setting and not four.
    Color accent = kNvidiaGreen;
    /// Off inherits appearance.title_color / detail_color, which is what keeps fifteen
    /// categories looking like one system.
    bool override_title_color = false;
    Color title_color{255, 255, 255, 255};
    bool override_detail_color = false;
    Color detail_color{168, 172, 176, 255};
    /// Colour each expanded placeholder in `placeholder_color` instead of the line's own colour,
    /// so a file name stands out from the sentence around it.
    bool color_placeholders = true;
    Color placeholder_color{235, 238, 240, 255};

    Fade fade{260, 220, 3200, 0.0f, 1.0f, Easing::EaseOutCubic};
    /// Empty inherits notifications.motion. A category may override it -- a warning that
    /// deserves to arrive differently should be able to.
    bool override_motion = false;
    Motion motion{};

    bool sound = false;
    std::string sound_file;

    /// Wrap the detail line rather than widening the box.
    bool wrap = false;
    int max_lines = 2;
    /// Higher priority survives when the queue overflows. A dropped-frames warning outliving a
    /// scene change is the right trade; the reverse is not.
    int priority = 0;
};

struct NotificationsConfig {
    Placement placement{true, Anchor::TopRight, 32.0f, 32.0f, false, Align::Right};
    int max_visible = 4;
    StackDirection stack = StackDirection::Down;
    float spacing = 8.0f;
    /// Used when box.auto_width is off, and as the minimum width when it is on. The reference
    /// toast is a consistent width regardless of its text, which a minimum reproduces.
    float width = 320.0f;
    float min_height = 56.0f;
    NotificationBoxStyle box{};
    float padding_x = 14.0f;
    float padding_y = 12.0f;
    /// Gap between the icon (or its tile) and the text column.
    float icon_gap = 12.0f;
    /// Scales both text sizes together, on top of appearance's own figures.
    float font_scale = 1.0f;
    Motion motion{};
    bool merge_duplicates = true;
    /// Events arriving within this window of the link coming up are absorbed into the initial
    /// synchronisation instead of announcing a state that was already true.
    int suppress_after_connect_ms = 1500;
    /// A hard floor between two toasts of the same category, so a script bug or a pathological
    /// OBS state cannot machine-gun the screen.
    int min_interval_ms = 250;

    NotificationStyle recording_started{};
    NotificationStyle recording_stopped{};
    NotificationStyle recording_paused{};
    NotificationStyle recording_resumed{};
    NotificationStyle recording_saved{};
    NotificationStyle replay_started{};
    NotificationStyle replay_stopped{};
    NotificationStyle replay_saved{};
    NotificationStyle stream_started{};
    NotificationStyle stream_stopped{};
    NotificationStyle stream_reconnecting{};
    NotificationStyle stream_reconnected{};
    NotificationStyle virtual_cam_started{};
    NotificationStyle virtual_cam_stopped{};
    NotificationStyle scene_changed{};
    NotificationStyle warning{};
    NotificationStyle obs_connected{};
    NotificationStyle obs_disconnected{};
};

/// The persistent on-screen mark that something is being captured.
///
/// The reference overlay keeps a small indicator visible for as long as a capture is running,
/// which is a different job from a notification: it answers "am I still recording?" at any
/// moment, rather than announcing a change. It is off by default because it is permanently on
/// screen, and that is the user's call to make.
struct StatusConfig {
    Placement placement{false, Anchor::TopRight, 32.0f, 32.0f, false, Align::Right};
    bool show_while_recording = true;
    bool show_while_streaming = true;
    /// Whether an armed replay buffer alone is enough to show the indicator. Off by default:
    /// the buffer is usually armed for the whole session, and a permanent dot is noise.
    bool show_while_replay_armed = false;
    bool show_timer = true;
    /// Blink the dot in time with the recording, as a hardware record light does.
    bool pulse = true;
    float pulse_hz = 0.6f;
    float pulse_depth = 0.45f;
    float dot_size = 9.0f;
    float font_size = 13.0f;
    float padding_x = 10.0f;
    float padding_y = 6.0f;
    float corner_radius = 2.0f;
    bool show_background = true;
    Color background{17, 17, 17, 190};
    Color text{235, 238, 240, 255};
    Color recording_color{235, 64, 52, 255};   ///< a record light is red, not the house accent
    Color streaming_color{118, 185, 0, 255};
    Color paused_color{240, 180, 40, 255};
    Fade fade{200, 200, 0, 0.0f, 1.0f, Easing::EaseOutCubic};
};

struct AnimationConfig {
    /// Master switch. Off pins every toast to its final position and uses a plain fade, which
    /// is the setting for someone who finds motion distracting rather than a debug aid.
    bool enabled = true;
    /// Animate the stack closing up when a toast above expires, instead of snapping.
    bool animate_restack = true;
    int restack_ms = 180;
    Easing restack_easing = Easing::EaseOutCubic;
};

struct IntegrationConfig {
    /// Empty means the default `obsn.v1.<user-sid>`. Overriding is a diagnostic aid, and must
    /// match the value set in the OBS script.
    std::string pipe_name;
    int reconnect_initial_ms = 250;
    int reconnect_max_ms = 5000;
    int stale_after_ms = 8000;
    int ping_interval_ms = 10000;
    bool auto_connect = true;
};

struct LoggingConfig {
    /// trace | debug | info | warn | error | off
    std::string level = "info";
    bool to_file = true;
    std::string file_name;  ///< empty means the default location
    int max_file_kb = 1024;
    /// Off by default. Recording paths are not written to the log unless this is switched on,
    /// and the settings UI states the consequence next to the checkbox.
    bool include_paths = false;
    bool show_diagnostics_overlay = false;
};

/// A repair the loader had to perform. Surfaced in the Diagnostics tab rather than swallowed.
struct ConfigIssue {
    enum class Severity { Info, Warning, Error } severity = Severity::Warning;
    std::string path;     ///< dotted path, e.g. "appearance.title_size"
    std::string message;
};

struct ConfigDiagnostics {
    std::vector<ConfigIssue> issues;
    bool migrated = false;
    int loaded_version = kConfigVersion;
    bool from_defaults = false;
    bool newer_than_supported = false;
    void add(ConfigIssue::Severity s, std::string path, std::string message) {
        issues.push_back({s, std::move(path), std::move(message)});
    }
    bool empty() const noexcept { return issues.empty(); }
};

struct Config {
    int config_version = kConfigVersion;
    GeneralConfig general;
    AppearanceConfig appearance;
    NotificationsConfig notifications;
    StatusConfig status;
    AnimationConfig animation;
    IntegrationConfig integration;
    LoggingConfig logging;
    /// Keys we did not recognise, kept so a round-trip through an older build is lossless.
    json::Object unknown;

    /// Defaults that are not merely zero -- the shipped look. Defined in config_defaults.cpp.
    static Config defaults();

    /// The style for one category, or nullptr for an event kind that never raises a toast.
    const NotificationStyle* style_for(EventKind) const noexcept;
    NotificationStyle* style_for(EventKind) noexcept;

    json::Value to_json() const;
    /// Total: always returns a usable Config. `diag` records every repair.
    static Config from_json(const json::Value&, ConfigDiagnostics& diag);

    /// Parses text, migrating older versions. Malformed input yields defaults plus an error.
    static Config parse(std::string_view text, ConfigDiagnostics& diag);
    std::string serialise() const;

    /// Clamps every numeric field into its supported range, reporting each clamp.
    void clamp(ConfigDiagnostics& diag);
};

/// Every category, in the order the settings UI lists them. Defined once so the UI, the
/// serialiser and the tests cannot disagree about what the set is.
struct CategoryEntry {
    const char* key;                                  ///< JSON key and UI identifier
    const char* label;                                ///< human-readable name
    NotificationStyle NotificationsConfig::*member;
    EventKind kind;
};
const std::vector<CategoryEntry>& categories();

/// Runs migrate_v(n) to v(n+1) steps in sequence. A document newer than kConfigVersion is left
/// untouched and flagged, never downgraded.
bool migrate(json::Value& doc, ConfigDiagnostics& diag);

}  // namespace obsn

#endif  // OBSN_CONFIG_HPP
