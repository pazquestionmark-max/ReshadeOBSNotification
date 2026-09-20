// SPDX-License-Identifier: MIT
#include "renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include <imgui.h>
// reshade.hpp must follow imgui.h: it supplies the inline definitions for ImGui:: and
// ImDrawList:: that route through ReShade's function table. imgui.h alone only declares them,
// so omitting this compiles cleanly and then fails at link with unresolved externals.
#include <reshade.hpp>

#include "icons.hpp"

namespace obsn::overlay {
namespace {

constexpr float kPi = 3.14159265358979323846f;

std::uint32_t packed(const Color& color, float opacity) {
    return color.with_alpha_scale(opacity).to_abgr();
}

/// The overlay's own typeface, when one is loaded. Owned by the add-on, not by the renderer.
FontEngine* g_fonts = nullptr;

/// Which measurement route last answered. Purely diagnostic -- the settings panel shows it, so
/// a measurement problem is visible in the overlay instead of having to be inferred from a
/// screenshot of misplaced text.
TextMetricsSource g_metrics_source = TextMetricsSource::Unknown;

/// Estimated width of a run of text, used only when ReShade's ImGui refuses to measure.
///
/// A proportional UI face averages a little over half its pixel size per glyph, so counting
/// codepoints (not bytes -- UTF-8 continuation bytes are not glyphs) and scaling gets within a
/// few percent. That is wrong in the last pixel and right in the ones that matter: everything
/// stays on screen, in the right order, roughly where it belongs.
float estimate_text_width(std::string_view text, float font_size) {
    std::size_t glyphs = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++glyphs;
    }
    return static_cast<float>(glyphs) * font_size * 0.52f;
}

/// Width of `text` when drawn at `font_size`.
///
/// This is the single most load-bearing number in the renderer: every right-aligned position is
/// `edge - width`, and every overflow budget is `box - width`. A zero here does not degrade the
/// layout, it inverts it -- toasts start at the right edge and grow off-screen, and text bodies
/// get a zero budget and vanish.
///
/// So measurement never returns zero for non-empty text. It only ever calls *through ReShade's
/// function table* -- reading ImGui structs directly assumes this build's imgui.h and ReShade's
/// own ImGui agree on every offset, which is not a safe assumption across builds -- it tries
/// both table entries that can answer, and estimates if neither does.
float measure_text(std::string_view text, float font_size) {
    if (text.empty() || font_size <= 0.0f) return 0.0f;

    // The overlay's own font answers from the very advance table its glyphs are drawn from, so
    // when it is live there is no way for alignment and rendering to disagree.
    if (g_fonts != nullptr) {
        const float own = g_fonts->measure(text, font_size);
        if (own > 0.0f) {
            g_metrics_source = TextMetricsSource::OwnFont;
            return own;
        }
    }

    const char* const begin = text.data();
    const char* const end = begin + text.size();

    // The namespace-level entry measures at the *current* font size, so scale the result.
    const float base = ImGui::GetFontSize();
    if (base > 0.0f) {
        const float width = ImGui::CalcTextSize(begin, end, false, -1.0f).x;
        if (width > 0.0f && std::isfinite(width)) {
            g_metrics_source = TextMetricsSource::CalcTextSize;
            return width * (font_size / base);
        }
    }

    // The per-font entry takes the size directly, which avoids the scaling round-trip.
    if (ImFont* font = ImGui::GetFont(); font != nullptr) {
        const float width = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, begin, end).x;
        if (width > 0.0f && std::isfinite(width)) {
            g_metrics_source = TextMetricsSource::CalcTextSizeA;
            return width;
        }
    }

    g_metrics_source = TextMetricsSource::Estimated;
    return estimate_text_width(text, font_size);
}

const MeasureFn& measure_fn() {
    static const MeasureFn fn = [](std::string_view t, float size) {
        return measure_text(t, size);
    };
    return fn;
}

std::int64_t now_ms_now() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/// A sample event of one kind, complete enough that every placeholder in the shipped templates
/// resolves. The preview is only honest if it goes through the real formatter with real values.
ObsEvent sample_event(EventKind kind, std::int64_t now_ms) {
    ObsEvent e;
    e.kind = kind;
    e.ts = now_ms;
    switch (output_of(kind)) {
        case OutputKind::Recording:
            e.path = "C:\\Users\\You\\Videos\\2026-09-20 21-14-03.mkv";
            e.duration_ms = 12 * 60 * 1000 + 7 * 1000;
            e.size_bytes = 1503238553;
            break;
        case OutputKind::ReplayBuffer:
            e.path = "C:\\Users\\You\\Videos\\Replay 2026-09-20 21-14-03.mkv";
            e.replay_seconds = 30;
            e.duration_ms = 30000;
            break;
        case OutputKind::Stream:
            e.service = "Twitch";
            e.duration_ms = 47 * 60 * 1000;
            e.attempt = 2;
            break;
        default:
            break;
    }
    e.scene = "Gameplay";
    e.previous_scene = "Starting soon";
    e.profile = "Untitled";
    if (kind == EventKind::Warning) e.detail = "Encoder overloaded: frames are being skipped";
    return e;
}

}  // namespace

TextMetricsSource text_metrics_source() noexcept { return g_metrics_source; }

const char* text_metrics_source_name(TextMetricsSource source) noexcept {
    switch (source) {
        case TextMetricsSource::Unknown: return "not yet measured";
        case TextMetricsSource::OwnFont: return "the overlay's own font";
        case TextMetricsSource::CalcTextSize: return "ImGui::CalcTextSize";
        case TextMetricsSource::CalcTextSizeA: return "ImFont::CalcTextSizeA";
        case TextMetricsSource::Estimated: return "estimated (ImGui did not answer)";
    }
    return "?";
}

ObsState preview_state(std::int64_t now_ms) {
    ObsState state;
    state.obs_running = true;
    state.obs_version = "30.2.3";
    state.script_version = OBSN_VERSION;
    state.profile = "Untitled";
    state.scene_collection = "Untitled";
    state.current_scene = "Gameplay";
    state.recording.state = OutputState::Active;
    // Twelve minutes in, so the status indicator shows a plausible timer rather than 0:00.
    state.recording.started_ms = now_ms - (12 * 60 * 1000 + 7 * 1000);
    state.recording.path = "C:\\Users\\You\\Videos\\2026-09-20 21-14-03.mkv";
    state.replay.state = OutputState::Active;
    state.replay.started_ms = now_ms - 40 * 60 * 1000;
    state.replay.duration_s = 30;
    state.updated_ms = now_ms;
    return state;
}

void Renderer::set_font_engine(FontEngine* fonts) noexcept { g_fonts = fonts; }

void Renderer::submit_events(const std::vector<ObsEvent>& events, const Config& config,
                             const ObsState& state, std::int64_t now_ms) {
    for (const ObsEvent& event : events) {
        // The connect edge seeds the suppression window before it is submitted, so the snapshot
        // that follows it cannot announce state that was already true.
        if (event.kind == EventKind::ScriptConnected) notifications_.note_connected(now_ms);
        notifications_.submit(event, config, state, now_ms);
    }
}

void Renderer::seed_preview_notifications(const Config& config, std::int64_t now_ms) {
    notifications_.clear();
    // The suppression window is explicitly cleared: a preview is asking to see the toasts, and
    // the post-connect rule exists to stop real events being announced, not sample ones.
    notifications_.note_connected(0);
    const ObsState state = preview_state(now_ms);

    // Stagger them, so the preview shows the stack filling the way it does in play rather than
    // four boxes appearing at once.
    std::int64_t at = now_ms;
    for (const CategoryEntry& entry : categories()) {
        const NotificationStyle& style = config.notifications.*(entry.member);
        if (!style.enabled) continue;
        notifications_.submit(sample_event(entry.kind, at), config, state, at);
        at += 60;
    }
    notifications_.drain_sounds();  // a preview never plays a sound
}

void Renderer::test_notification(EventKind kind, const Config& config, std::int64_t now_ms) {
    notifications_.note_connected(0);
    notifications_.submit(sample_event(kind, now_ms), config, preview_state(now_ms), now_ms);
    notifications_.drain_sounds();
}

void Renderer::draw_text(ImDrawList* dl, const Config& config, float x, float y, float size,
                         std::uint32_t color, std::string_view text) {
    if (text.empty() || size <= 0.0f) return;
    const AppearanceConfig& a = config.appearance;

    const bool own_font = g_fonts != nullptr && g_fonts->ready_at(size);
    const auto put = [&](float px, float py, std::uint32_t c) {
        if (own_font) {
            g_fonts->draw(dl, px, py, size, c, text);
        } else {
            dl->AddText(nullptr, size, ImVec2(px, py), c, text.data(), text.data() + text.size());
        }
    };

    // An outline is eight extra draws and is the only thing that stays readable over *any*
    // background rather than most of them, so it takes precedence over the cheaper shadow when
    // both are on -- drawing both would just thicken the outline unevenly.
    if (a.text_outline && a.text_outline_thickness > 0.0f) {
        const float t = a.text_outline_thickness;
        const std::uint32_t outline = a.text_outline_color.to_abgr();
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                put(x + static_cast<float>(dx) * t, y + static_cast<float>(dy) * t, outline);
            }
        }
    } else if (a.text_shadow && a.text_shadow_offset > 0.0f) {
        put(x + a.text_shadow_offset, y + a.text_shadow_offset, a.text_shadow_color.to_abgr());
    }
    put(x, y, color);
}

void Renderer::draw_highlighted(ImDrawList* dl, const Config& config, float x, float y,
                                float size, const NotificationLine& line, std::string_view text,
                                std::uint32_t color, std::uint32_t highlight) {
    if (text.empty()) return;
    if (line.highlights.empty()) {
        draw_text(dl, config, x, y, size, color, text);
        return;
    }

    // Highlights are byte offsets into the whole formatted line, but `text` may be one wrapped
    // piece of it. They are located in the piece by value rather than by offset, which is what
    // keeps a highlighted file name correctly coloured after the line has been broken up.
    float cursor = x;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t best = std::string_view::npos;
        std::size_t best_len = 0;
        for (const NotificationLine::Highlight& h : line.highlights) {
            if (h.end <= h.begin || h.end > line.text.size()) continue;
            const std::string_view value(line.text.data() + h.begin, h.end - h.begin);
            const std::size_t found = text.find(value, at);
            if (found == std::string_view::npos) continue;
            // Earliest wins; on a tie the longer run wins, so a placeholder nested inside
            // another's text does not truncate it.
            if (found < best || (found == best && value.size() > best_len)) {
                best = found;
                best_len = value.size();
            }
        }
        if (best == std::string_view::npos) {
            const std::string_view rest = text.substr(at);
            draw_text(dl, config, cursor, y, size, color, rest);
            return;
        }
        if (best > at) {
            const std::string_view before = text.substr(at, best - at);
            draw_text(dl, config, cursor, y, size, color, before);
            cursor += measure_text(before, size);
        }
        const std::string_view value = text.substr(best, best_len);
        draw_text(dl, config, cursor, y, size, highlight, value);
        cursor += measure_text(value, size);
        at = best + best_len;
    }
}

void Renderer::draw_notifications(ImDrawList* dl, const Config& config, const Viewport& viewport,
                                  float opacity, std::int64_t now_ms, float dt_ms) {
    const NotificationsConfig& nc = config.notifications;
    if (!nc.placement.visible || notifications_.items().empty()) return;

    const NotificationBoxStyle& box = nc.box;
    const float scale = config.general.scale;
    const float title_size = config.appearance.title_size * scale * nc.font_scale;
    const float detail_size = config.appearance.detail_size * scale * nc.font_scale;
    const float pad_x = nc.padding_x * scale;
    const float pad_y = nc.padding_y * scale;
    const float icon_size = config.appearance.icon_size * scale;
    const float icon_gap = nc.icon_gap * scale;
    const float line_gap = config.appearance.line_gap * scale;
    const float spacing = nc.spacing * scale;
    const float min_width = nc.width * scale;
    const float max_width = std::min(box.max_width * scale, std::max(96.0f, viewport.width - 16.0f));

    const bool has_tile = box.accent_style == AccentStyle::Tile ||
                          box.accent_style == AccentStyle::BarAndTile;
    const bool has_bar = box.accent_style == AccentStyle::Bar ||
                         box.accent_style == AccentStyle::BarAndTile;
    const float tile_w = has_tile ? box.accent_tile_width * scale : 0.0f;
    const float bar_w = has_bar ? box.accent_bar_width * scale : 0.0f;

    // The icon lives inside the tile when there is one, and beside the text when there is not.
    // Either way the text column starts after it, which is what keeps the two arrangements
    // looking like the same design rather than two.
    const float lead_w = has_tile ? tile_w + pad_x
                                  : bar_w + pad_x + (icon_size + icon_gap);
    const float text_budget = std::max(24.0f, max_width - lead_w - pad_x);

    // Pass one: measure every toast, because their heights differ once a detail line wraps and
    // the stack cannot be positioned until they are known.
    toasts_.clear();
    for (Notification& notification : notifications_.mutable_items()) {
        const float fade = notification.opacity(now_ms);
        if (fade <= 0.003f) continue;

        Toast t;
        t.item = &notification;
        t.alpha = opacity * fade;
        t.tile_w = tile_w;
        t.bar_w = bar_w;

        if (notification.repeat_count > 1) {
            t.badge = "x" + std::to_string(notification.repeat_count);
            t.badge_w = measure_text(t.badge, detail_size) + icon_gap;
        }

        const float title_wanted = measure_text(notification.title.text, title_size);
        t.title_fits = title_wanted <= text_budget - t.badge_w;
        t.title_w = std::min(title_wanted, std::max(16.0f, text_budget - t.badge_w));

        t.detail_lines.clear();
        t.detail_w = 0.0f;
        if (!notification.detail.text.empty()) {
            const float detail_wanted = measure_text(notification.detail.text, detail_size);
            if (notification.wrap && detail_wanted > text_budget) {
                const FittedText fitted = fit_text(notification.detail.text, text_budget,
                                                   detail_size, OverflowMode::Wrap, 1.0f,
                                                   measure_fn());
                t.detail_lines = fitted.lines;
                const std::size_t cap =
                    static_cast<std::size_t>(std::max(1, notification.max_lines));
                if (t.detail_lines.size() > cap) {
                    t.detail_lines.resize(cap);
                    if (!t.detail_lines.back().empty()) t.detail_lines.back() += "...";
                }
                for (const std::string& line : t.detail_lines) {
                    t.detail_w = std::max(t.detail_w, measure_text(line, detail_size));
                }
                // Each line was wrapped to the budget, so none of them overflows.
                t.detail_fits = true;
            } else {
                t.detail_fits = detail_wanted <= text_budget;
                t.detail_w = std::min(detail_wanted, text_budget);
                t.detail_lines.assign(1, notification.detail.text);
            }
        }

        const float text_w = std::max(t.title_w + t.badge_w, t.detail_w);
        const float wanted = lead_w + text_w + pad_x;
        t.box_w = box.auto_width ? std::clamp(wanted, min_width, max_width) : min_width;

        float text_h = title_size;
        if (!t.detail_lines.empty()) {
            text_h += line_gap + detail_size * static_cast<float>(t.detail_lines.size());
        }
        t.box_h = std::max(nc.min_height * scale, pad_y * 2.0f + text_h);

        toasts_.push_back(std::move(t));
    }
    if (toasts_.empty()) return;

    float stack_height = 0.0f;
    for (const Toast& t : toasts_) stack_height += t.box_h + spacing;
    stack_height -= spacing;

    // Placement is resolved once against the widest a toast may get. Every anchor puts its own
    // edge at a position that does not depend on the box width -- a right anchor pins the right
    // edge -- so this frame of reference stays correct though each toast is a different size.
    const Rect area = resolve_placement(nc.placement, max_width, stack_height, viewport);

    // Pass two: assign each toast its slot in the stack and let it travel there. The target is
    // recomputed every frame and the toast eases towards it, which is what makes the stack
    // close up smoothly when one above it expires instead of everything snapping upwards.
    float target = 0.0f;
    const std::size_t count = toasts_.size();
    for (std::size_t index = 0; index < count; ++index) {
        Toast& t = toasts_[nc.stack == StackDirection::Down ? index : count - 1 - index];
        t.item->settle(target, dt_ms, config.animation);
        target += t.box_h + spacing;
    }

    for (const Toast& t : toasts_) {
        float box_x = area.x;
        if (nc.placement.align == Align::Right) {
            box_x = area.right() - t.box_w;
        } else if (nc.placement.align == Align::Center) {
            box_x = area.x + (area.w - t.box_w) * 0.5f;
        }
        draw_one_toast(dl, config, t, box_x, area.y + t.item->stack_offset, now_ms);
    }
    stats_.visible_toasts = toasts_.size();
}

void Renderer::draw_one_toast(ImDrawList* dl, const Config& config, const Toast& t, float box_x,
                              float box_y, std::int64_t now_ms) {
    const NotificationsConfig& nc = config.notifications;
    const NotificationBoxStyle& box = nc.box;
    const Notification& n = *t.item;
    const float scale = config.general.scale;

    // The entry and exit travel. Applied here rather than baked into the stack position, so a
    // toast sliding out does not drag the ones below it with it.
    Rect geometry{box_x, box_y, t.box_w, t.box_h};
    bool entering = true;
    const float progress = n.animation_progress(now_ms, entering);
    const MotionOffset offset =
        motion_offset(n.motion, nc.placement.anchor, geometry, progress, entering);

    float x = box_x + offset.dx;
    float y = box_y + offset.dy;
    float w = t.box_w;
    float h = t.box_h;
    if (offset.scale != 1.0f) {
        // Scaling is about the box's own centre, so a growing toast does not appear to also
        // move towards its anchor.
        const float cx = x + w * 0.5f;
        const float cy = y + h * 0.5f;
        w *= offset.scale;
        h *= offset.scale;
        x = cx - w * 0.5f;
        y = cy - h * 0.5f;
    }

    const float alpha = t.alpha;
    const float radius = box.corner_radius * scale;

    if (box.shadow) {
        draw_shadow(dl, x, y, w, h, radius, box.shadow_color.to_abgr(),
                    box.shadow_size * scale, box.shadow_offset_y * scale);
    }

    std::uint32_t edge = 0u;
    if (box.border == NotificationBorder::Accent) {
        edge = packed(n.accent, alpha * box.border_accent_opacity);
    } else if (box.border == NotificationBorder::Custom) {
        edge = packed(box.border_color, alpha);
    }
    draw_panel(dl, x, y, w, h, radius, packed(box.background, alpha), edge,
               box.border_thickness * scale);
    if (box.background_gradient.a > 0) {
        draw_vertical_gradient(dl, x, y, w, h, packed(box.background_gradient, alpha * 0.0f),
                               packed(box.background_gradient, alpha));
    }

    const bool has_tile = box.accent_style == AccentStyle::Tile ||
                          box.accent_style == AccentStyle::BarAndTile;
    const bool has_bar = box.accent_style == AccentStyle::Bar ||
                         box.accent_style == AccentStyle::BarAndTile;
    const float tile_w = has_tile ? t.tile_w : 0.0f;
    const float bar_w = has_bar ? t.bar_w : 0.0f;

    if (has_tile && tile_w > 0.0f) {
        // The tile is the box's full height with only its leading corners rounded, so it reads
        // as part of the panel rather than a square sitting on top of one.
        const float r = std::min(radius, std::min(tile_w, h) * 0.5f);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + tile_w, y + h),
                          packed(n.accent, alpha * box.accent_tile_opacity), r,
                          ImDrawFlags_RoundCornersLeft);
    }
    if (has_bar && bar_w > 0.0f) {
        const float bar_x = x + tile_w;
        dl->AddRectFilled(ImVec2(bar_x, y), ImVec2(bar_x + bar_w, y + h),
                          packed(n.accent, alpha), 0.0f);
    }

    const float icon_size = config.appearance.icon_size * scale;
    const float pad_x = nc.padding_x * scale;
    const float pad_y = nc.padding_y * scale;
    const float icon_gap = nc.icon_gap * scale;

    if (n.icon != IconShape::None && icon_size > 0.0f) {
        // On a filled tile the icon takes the tile's contrasting colour: an accent-coloured
        // glyph on an accent-coloured tile is invisible, and that is a setting people would
        // otherwise have to discover by turning the tile on and seeing nothing.
        const Color icon_color = has_tile && box.accent_tile_opacity > 0.5f
                                     ? box.tile_icon_color
                                     : n.accent;
        const float icon_cx = has_tile ? x + tile_w * 0.5f
                                       : x + bar_w + pad_x + icon_size * 0.5f;
        draw_icon(dl, n.icon, icon_cx, y + h * 0.5f, icon_size, packed(icon_color, alpha),
                  1.6f * scale);
    }

    const float title_size = config.appearance.title_size * scale * nc.font_scale;
    const float detail_size = config.appearance.detail_size * scale * nc.font_scale;
    const float line_gap = config.appearance.line_gap * scale;
    const float text_x = has_tile ? x + tile_w + pad_x
                                  : x + bar_w + pad_x + icon_size + icon_gap;

    float text_h = title_size;
    if (!t.detail_lines.empty()) {
        text_h += line_gap + detail_size * static_cast<float>(t.detail_lines.size());
    }
    // The text block is centred vertically, which is what keeps a one-line toast and a
    // three-line one looking like the same component at different heights.
    float text_y = y + (h - text_h) * 0.5f;
    if (text_y < y + pad_y) text_y = y + pad_y;

    const std::uint32_t highlight = packed(n.placeholder_color, alpha);

    if (!n.title.text.empty()) {
        if (t.title_fits) {
            draw_highlighted(dl, config, text_x, text_y, title_size, n.title, n.title.text,
                             packed(n.title.color, alpha), highlight);
        } else {
            const FittedText fitted = fit_text(n.title.text, t.title_w, title_size,
                                               OverflowMode::Ellipsis, 0.75f, measure_fn());
            draw_text(dl, config, text_x, text_y, title_size, packed(n.title.color, alpha),
                      fitted.text);
        }
    }

    if (!t.badge.empty()) {
        // The repeat count has its own reserved width, so it sits beside the title rather than
        // on top of it.
        const float badge_w = measure_text(t.badge, detail_size);
        draw_text(dl, config, x + w - pad_x - badge_w, text_y, detail_size,
                  packed(n.detail.color, alpha), t.badge);
    }

    float line_y = text_y + title_size + line_gap;
    for (std::size_t i = 0; i < t.detail_lines.size(); ++i) {
        const std::string& line = t.detail_lines[i];
        if (t.detail_fits) {
            draw_highlighted(dl, config, text_x, line_y, detail_size, n.detail, line,
                             packed(n.detail.color, alpha), highlight);
        } else {
            const FittedText fitted = fit_text(line, t.detail_w, detail_size,
                                               OverflowMode::Ellipsis, 0.75f, measure_fn());
            draw_text(dl, config, text_x, line_y, detail_size, packed(n.detail.color, alpha),
                      fitted.text);
        }
        line_y += detail_size;
    }
}

void Renderer::draw_status(ImDrawList* dl, const Config& config, const ObsState& state,
                           const Viewport& viewport, float opacity, std::int64_t now_ms) {
    const StatusConfig& sc = config.status;
    const StatusLine line = status_line(state, config, now_ms);

    if (line.visible != status_was_visible_) {
        status_was_visible_ = line.visible;
        status_since_ms_ = now_ms;
    }
    if (!line.visible) return;

    const float scale = config.general.scale;
    const float font = sc.font_size * scale;
    const float dot = sc.dot_size * scale;
    const float pad_x = sc.padding_x * scale;
    const float pad_y = sc.padding_y * scale;
    const float gap = dot > 0.0f ? pad_x * 0.6f : 0.0f;

    // The indicator fades in on the same envelope a toast uses, so the whole overlay behaves as
    // one thing. `hold_ms` is ignored here: this element stays until the state changes.
    float alpha = opacity;
    if (sc.fade.in_ms > 0) {
        const float t = static_cast<float>(now_ms - status_since_ms_) /
                        static_cast<float>(sc.fade.in_ms);
        alpha *= sc.fade.start_opacity +
                 (sc.fade.end_opacity - sc.fade.start_opacity) * ease(sc.fade.easing, t);
    }
    if (alpha <= 0.003f) return;

    const float text_w = measure_text(line.text, font);
    const float w = pad_x * 2.0f + dot + gap + text_w;
    const float h = std::max(dot, font) + pad_y * 2.0f;
    const Rect box = resolve_placement(sc.placement, w, h, viewport);

    if (sc.show_background) {
        draw_panel(dl, box.x, box.y, box.w, box.h, sc.corner_radius * scale,
                   packed(sc.background, alpha), 0u, 0.0f);
    }

    float dot_alpha = alpha;
    if (line.pulsing && sc.pulse_hz > 0.0f && sc.pulse_depth > 0.0f) {
        // A slow sine rather than a square blink: a hard on/off at the edge of vision is what
        // makes a recording light distracting, and the whole point of this element is that it
        // can be ignored until it is wanted.
        const float phase = static_cast<float>(now_ms % 100000) * 0.001f * sc.pulse_hz * 2.0f * kPi;
        const float wave = (std::sin(phase) + 1.0f) * 0.5f;
        dot_alpha *= 1.0f - sc.pulse_depth * (1.0f - wave);
    }

    const float cy = box.y + box.h * 0.5f;
    if (dot > 0.0f) {
        // Paused shows the two-bar mark instead of a dot: a still dot and a blinking dot are
        // too easy to confuse at a glance, and "paused" is exactly when that matters.
        draw_icon(dl, line.paused ? IconShape::Pause : IconShape::Record,
                  box.x + pad_x + dot * 0.5f, cy, dot, packed(line.dot_color, dot_alpha),
                  1.5f * scale);
    }
    if (!line.text.empty()) {
        draw_text(dl, config, box.x + pad_x + dot + gap, cy - font * 0.5f, font,
                  packed(sc.text, alpha), line.text);
    }
}

void Renderer::draw(ImDrawList* dl, const Config& config, const OverlayFrame& frame,
                    const Viewport& viewport, std::int64_t now_ms, const ObsState* preview) {
    if (dl == nullptr || !config.general.enabled) return;
    if (viewport.width < 1.0f || viewport.height < 1.0f) return;

    const std::int64_t started = now_ms_now();
    const float dt_ms = last_frame_ms_ > 0
                            ? static_cast<float>(std::clamp<std::int64_t>(now_ms - last_frame_ms_,
                                                                         0, 250))
                            : 0.0f;
    last_frame_ms_ = now_ms;

    const ObsState& state = preview != nullptr ? *preview : frame.state;

    // With OBS closed there is nothing true to say, so by default nothing is said. Toasts
    // already in flight are still allowed to finish: cutting one off mid-slide because the
    // script exited would look like a bug.
    const bool link_usable = state.obs_running || config.general.show_when_obs_closed ||
                             preview != nullptr;

    notifications_.tick(now_ms, config);

    const float opacity = config.general.master_opacity;
    if (link_usable) {
        draw_status(dl, config, state, viewport, opacity, now_ms);
    } else {
        status_was_visible_ = false;
    }
    draw_notifications(dl, config, viewport, opacity, now_ms, dt_ms);

    ++stats_.frames;
    stats_.last_draw_ms = static_cast<float>(now_ms_now() - started);
    stats_.draw_commands = static_cast<std::size_t>(dl->CmdBuffer.Size);
}

}  // namespace obsn::overlay
