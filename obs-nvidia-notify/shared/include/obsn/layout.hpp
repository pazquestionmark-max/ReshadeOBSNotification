// SPDX-License-Identifier: MIT
// Geometry, text fitting and template expansion. Pure functions over (state, config, viewport)
// with text measurement injected, so the whole visual pipeline short of the actual draw calls is
// testable without a GPU -- which is how "correct at any resolution and aspect ratio" is
// established in CI rather than only by looking at it.
#ifndef OBSN_LAYOUT_HPP
#define OBSN_LAYOUT_HPP

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "obsn/config.hpp"
#include "obsn/model.hpp"

namespace obsn {

struct Viewport {
    float width = 1920.0f;
    float height = 1080.0f;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const noexcept { return x + w; }
    float bottom() const noexcept { return y + h; }
    bool contains(float px, float py) const noexcept {
        return px >= x && px <= right() && py >= y && py <= bottom();
    }
};

/// Measures the advance width of `text` at `font_size` pixels. Supplied by the renderer (the
/// overlay's own font) in production and by a deterministic stub in tests.
using MeasureFn = std::function<float(std::string_view text, float font_size)>;

/// Top-left corner for a box of size (w,h) placed at `p` within `vp`.
/// Percentage offsets are fractions of the viewport; pixel offsets are absolute. The anchor
/// determines which viewport corner or edge the offset is measured from and which way the box
/// grows, so a bottom-right anchored element stays bottom-right at any resolution.
Rect resolve_placement(const Placement& p, float w, float h, const Viewport& vp) noexcept;

/// Horizontal start for text of width `text_w` inside a box of width `box_w`.
float align_offset(Align a, float box_w, float text_w) noexcept;

/// Which screen edge an anchor sits against, and therefore which way a toast slides in from.
/// A centred anchor has no edge of its own; it takes the vertical one, because a toast centred
/// horizontally at the top of the screen still reads as arriving from the top.
enum class SlideAxis { FromLeft, FromRight, FromTop, FromBottom };
SlideAxis slide_axis_for(Anchor) noexcept;

/// The offset a toast is drawn at, given how far through its entry or exit it is.
///
/// `t` is 0 at the start of the animation and 1 at its end; `entering` picks the easing and the
/// direction of travel. `box` is the toast's own size, which is what a zero `distance` means
/// "travel my own width" against.
struct MotionOffset {
    float dx = 0.0f;
    float dy = 0.0f;
    float scale = 1.0f;
};
MotionOffset motion_offset(const Motion& motion, Anchor anchor, const Rect& box, float t,
                           bool entering) noexcept;

struct FittedText {
    std::string text;               ///< possibly truncated, with the ellipsis already appended
    std::vector<std::string> lines; ///< one entry unless the mode is Wrap
    float font_scale = 1.0f;        ///< < 1 only for OverflowMode::Shrink
    float width = 0.0f;
    bool truncated = false;
    /// Pixels to shift left this frame for OverflowMode::Scroll; 0 for every other mode.
    float scroll_offset = 0.0f;
};

/// Applies an overflow strategy so a long file name cannot break the layout.
/// `time_s` drives Scroll; ignored otherwise.
FittedText fit_text(std::string_view text, float max_width, float font_size, OverflowMode mode,
                    float min_font_scale, const MeasureFn& measure, float time_s = 0.0f);

/// The values a notification's title and detail templates can refer to.
///
/// Every field is a string, including the numbers: the formatter's job is substitution, and a
/// value OBS could not supply is an empty string rather than a zero, so "{duration}" on an event
/// with no duration disappears instead of claiming 0:00.
struct FormatValues {
    std::string file;            ///< the file name alone
    std::string path;            ///< the full path
    std::string folder;          ///< the directory the file is in
    std::string duration;        ///< "12:07"
    std::string size;            ///< "1.4 GB"
    std::string scene;
    std::string previous_scene;
    std::string profile;
    std::string service;
    std::string reason;
    std::string detail;
    std::string time;            ///< wall-clock, as the producer formatted it
    std::string attempt;
    std::string replay_seconds;
    std::string elapsed;         ///< how long the output has been running
};

/// Substitutes {file} {path} {folder} {duration} {size} {scene} {previous_scene} {profile}
/// {service} {reason} {detail} {time} {attempt} {replay_seconds} {elapsed}.
///
/// Unknown placeholders are left exactly as written, so a typo is visible in the overlay rather
/// than silently swallowed. A placeholder whose value is empty collapses along with any single
/// run of spaces beside it, which is what stops "Saved {file}" leaving a trailing space when
/// OBS did not report a name.
std::string format_template(std::string_view tmpl, const FormatValues& v);

/// Which placeholder a run of the formatted text came from, so the renderer can colour it.
enum class FormatField {
    None, File, Path, Folder, Duration, Size, Scene, PreviousScene, Profile, Service,
    Reason, Detail, Time, Attempt, ReplaySeconds, Elapsed,
};

struct FormatSpan {
    std::size_t begin = 0;   ///< byte offset into the formatted string
    std::size_t end = 0;
    FormatField field = FormatField::None;
};

/// Same expansion, but also records where each substituted value landed. Anything not covered by
/// a span is literal text from the template.
std::string format_template(std::string_view tmpl, const FormatValues& v,
                            std::vector<FormatSpan>& spans);

/// Builds the substitution values for one event, given the state it happened against.
FormatValues values_for(const ObsEvent& event, const ObsState& state, std::int64_t now_ms);

/// What the status indicator should say right now, or an empty text when it should not be shown.
struct StatusLine {
    bool visible = false;
    std::string text;         ///< "12:07", or empty when the timer is off
    Color dot_color{};
    /// True while recording is paused, so the renderer can hold the pulse still rather than
    /// blinking at a recording that is not advancing.
    bool paused = false;
    bool pulsing = false;
};
StatusLine status_line(const ObsState& state, const Config& config, std::int64_t now_ms);

}  // namespace obsn

#endif  // OBSN_LAYOUT_HPP
