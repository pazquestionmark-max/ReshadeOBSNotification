// SPDX-License-Identifier: MIT
// The overlay renderer.
//
// Runs inside ReShade's ImGui frame, on the game's render thread. Its contract:
//   * never block -- it takes no lock that any other thread holds for long, and never touches a
//     pipe handle;
//   * never allocate in steady state -- buffers are reserved once and reused;
//   * create no GPU resource of its own -- it only appends to ImGui's background draw list,
//     which ReShade already submits, so device resets and resolution changes need no handling
//     here. (The font engine does own a texture, and handles that itself.)
#ifndef OBSN_RENDERER_HPP
#define OBSN_RENDERER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "font_engine.hpp"
#include "obsn/config.hpp"
#include "obsn/layout.hpp"
#include "obsn/notifications.hpp"
#include "obsn/overlay_client.hpp"

struct ImDrawList;

namespace obsn::overlay {

/// How ReShade's ImGui answered the last text measurement.
///
/// Text width drives every right-aligned position and every overflow budget, and ReShade's ImGui
/// is a different build from the one this add-on compiles against, so measurement is not
/// something that can simply be assumed to work. `Estimated` means neither table entry returned
/// a usable width and the overlay is running on a codepoint estimate -- still readable, but the
/// edges will be a few pixels out. The diagnostics panel shows which route answered.
enum class TextMetricsSource { Unknown, OwnFont, CalcTextSize, CalcTextSizeA, Estimated };

TextMetricsSource text_metrics_source() noexcept;
const char* text_metrics_source_name(TextMetricsSource source) noexcept;

/// A sample OBS state for the settings preview, so the preview can show the status indicator
/// and the {elapsed} placeholder without anything actually being recorded.
ObsState preview_state(std::int64_t now_ms);

struct FrameStats {
    float last_draw_ms = 0.0f;
    std::uint64_t frames = 0;
    std::size_t visible_toasts = 0;
    std::size_t draw_commands = 0;
};

class Renderer {
public:
    /// Draws one frame. `now_ms` is wall-clock; `viewport` comes from ReShade's own API.
    /// When `preview` is set, that state is drawn instead of the live one -- used by the
    /// settings window's live preview, which never fabricates an OBS event on the wire.
    void draw(ImDrawList* draw_list, const Config& config, const OverlayFrame& frame,
              const Viewport& viewport, std::int64_t now_ms, const ObsState* preview);

    /// The overlay's own typeface, or nullptr to draw with ReShade's font. Set once at startup;
    /// the renderer only reads it, and falls back whenever the engine cannot serve a size.
    void set_font_engine(FontEngine* fonts) noexcept;

    /// Feeds events into the notification queue. Called before draw, on the same thread.
    void submit_events(const std::vector<ObsEvent>& events, const Config& config,
                       const ObsState& state, std::int64_t now_ms);

    void note_connected(std::int64_t now_ms) { notifications_.note_connected(now_ms); }

    /// Fills the queue with one of every enabled category, for the settings preview, so the user
    /// can see where each type lands and how it is styled without waiting for the events to
    /// happen for real. Uses the same formatting and lifecycle path as live notifications -- it
    /// is the real pipeline fed sample events, not a separate mock renderer.
    void seed_preview_notifications(const Config& config, std::int64_t now_ms);
    /// Raises one sample toast of a single category, for the per-category "Test" button.
    void test_notification(EventKind kind, const Config& config, std::int64_t now_ms);
    /// True once every seeded notification has aged out, so the caller can re-seed.
    bool notifications_empty() const noexcept { return notifications_.items().empty(); }
    void clear_notifications() { notifications_.clear(); }

    const FrameStats& stats() const noexcept { return stats_; }

private:
    /// One laid-out toast. Kept as a member so the per-frame layout does not allocate afresh
    /// every frame on the render thread.
    struct Toast {
        Notification* item = nullptr;
        float alpha = 0.0f;
        float title_w = 0.0f;
        float detail_w = 0.0f;
        float box_w = 0.0f;
        float box_h = 0.0f;
        float tile_w = 0.0f;
        float bar_w = 0.0f;
        /// True when the text was measured to fit, so nothing may be ellipsised.
        bool title_fits = true;
        bool detail_fits = true;
        std::string badge;
        float badge_w = 0.0f;
        std::vector<std::string> detail_lines;
    };

    void draw_notifications(ImDrawList* dl, const Config& config, const Viewport& viewport,
                            float opacity, std::int64_t now_ms, float dt_ms);
    void draw_one_toast(ImDrawList* dl, const Config& config, const Toast& toast, float box_x,
                        float box_y, std::int64_t now_ms);
    void draw_status(ImDrawList* dl, const Config& config, const ObsState& state,
                     const Viewport& viewport, float opacity, std::int64_t now_ms);
    /// Draws a line in `color`, with the highlighted runs in `highlight`.
    void draw_highlighted(ImDrawList* dl, const Config& config, float x, float y, float size,
                          const NotificationLine& line, std::string_view text,
                          std::uint32_t color, std::uint32_t highlight);
    void draw_text(ImDrawList* dl, const Config& config, float x, float y, float size,
                   std::uint32_t color, std::string_view text);

    NotificationQueue notifications_;
    std::vector<Toast> toasts_;
    FrameStats stats_;
    std::int64_t last_frame_ms_ = 0;
    /// Seeded so the first status indicator does not appear mid-fade.
    std::int64_t status_since_ms_ = 0;
    bool status_was_visible_ = false;
    std::string text_scratch_;
};

}  // namespace obsn::overlay

#endif  // OBSN_RENDERER_HPP
