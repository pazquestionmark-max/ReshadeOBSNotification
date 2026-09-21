// SPDX-License-Identifier: MIT
#include "obsn/layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace obsn {
namespace {

/// Advances past one UTF-8 sequence, so truncation never splits a character in half and never
/// runs past the end on malformed input.
std::size_t next_utf8(std::string_view s, std::size_t i) noexcept {
    if (i >= s.size()) return s.size();
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    if ((c & 0xF8) == 0xF0) len = 4;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xE0) == 0xC0) len = 2;
    return std::min(s.size(), i + len);
}

constexpr const char* kEllipsis = "...";

struct Placeholder {
    const char* name;
    FormatField field;
    const std::string FormatValues::*member;
};

const Placeholder kPlaceholders[] = {
    {"file", FormatField::File, &FormatValues::file},
    {"path", FormatField::Path, &FormatValues::path},
    {"folder", FormatField::Folder, &FormatValues::folder},
    {"duration", FormatField::Duration, &FormatValues::duration},
    {"size", FormatField::Size, &FormatValues::size},
    {"scene", FormatField::Scene, &FormatValues::scene},
    {"previous_scene", FormatField::PreviousScene, &FormatValues::previous_scene},
    {"profile", FormatField::Profile, &FormatValues::profile},
    {"service", FormatField::Service, &FormatValues::service},
    {"reason", FormatField::Reason, &FormatValues::reason},
    {"detail", FormatField::Detail, &FormatValues::detail},
    {"time", FormatField::Time, &FormatValues::time},
    {"attempt", FormatField::Attempt, &FormatValues::attempt},
    {"replay_seconds", FormatField::ReplaySeconds, &FormatValues::replay_seconds},
    {"elapsed", FormatField::Elapsed, &FormatValues::elapsed},
};

const Placeholder* find_placeholder(std::string_view name) noexcept {
    for (const Placeholder& p : kPlaceholders) {
        if (name == p.name) return &p;
    }
    return nullptr;
}

std::string to_str(long long v) {
    char buffer[24];
    const int n = std::snprintf(buffer, sizeof(buffer), "%lld", v);
    return std::string(buffer, static_cast<std::size_t>(n > 0 ? n : 0));
}

std::string folder_of(std::string_view path) {
    const std::size_t cut = path.find_last_of("/\\");
    if (cut == std::string_view::npos) return {};
    return std::string(path.substr(0, cut));
}

}  // namespace

Rect resolve_placement(const Placement& p, float w, float h, const Viewport& vp) noexcept {
    const float ox = p.percent ? p.x * vp.width : p.x;
    const float oy = p.percent ? p.y * vp.height : p.y;

    Rect r;
    r.w = w;
    r.h = h;

    switch (p.anchor) {
        case Anchor::TopLeft:
        case Anchor::CenterLeft:
        case Anchor::BottomLeft:
            r.x = ox;
            break;
        case Anchor::TopCenter:
        case Anchor::Center:
        case Anchor::BottomCenter:
            // A centred anchor measures its offset from the centre, so a non-zero x nudges the
            // element off-centre rather than being ignored.
            r.x = (vp.width - w) * 0.5f + ox;
            break;
        case Anchor::TopRight:
        case Anchor::CenterRight:
        case Anchor::BottomRight:
            r.x = vp.width - ox - w;
            break;
    }

    switch (p.anchor) {
        case Anchor::TopLeft:
        case Anchor::TopCenter:
        case Anchor::TopRight:
            r.y = oy;
            break;
        case Anchor::CenterLeft:
        case Anchor::Center:
        case Anchor::CenterRight:
            r.y = (vp.height - h) * 0.5f + oy;
            break;
        case Anchor::BottomLeft:
        case Anchor::BottomCenter:
        case Anchor::BottomRight:
            r.y = vp.height - oy - h;
            break;
    }
    return r;
}

float align_offset(Align a, float box_w, float text_w) noexcept {
    switch (a) {
        case Align::Left: return 0.0f;
        case Align::Center: return std::max(0.0f, (box_w - text_w) * 0.5f);
        case Align::Right: return std::max(0.0f, box_w - text_w);
    }
    return 0.0f;
}

SlideAxis slide_axis_for(Anchor anchor) noexcept {
    switch (anchor) {
        case Anchor::TopLeft:
        case Anchor::CenterLeft:
        case Anchor::BottomLeft:
            return SlideAxis::FromLeft;
        case Anchor::TopRight:
        case Anchor::CenterRight:
        case Anchor::BottomRight:
            return SlideAxis::FromRight;
        case Anchor::TopCenter:
            return SlideAxis::FromTop;
        case Anchor::BottomCenter:
            return SlideAxis::FromBottom;
        case Anchor::Center:
            // Dead centre has no edge to come from. Down is the least surprising of the four,
            // and matches what the vertical anchors either side of it do.
            return SlideAxis::FromTop;
    }
    return SlideAxis::FromRight;
}

MotionOffset motion_offset(const Motion& motion, Anchor anchor, const Rect& box, float t,
                           bool entering) noexcept {
    MotionOffset out;
    if (motion.kind == MotionKind::None) return out;
    if (!entering && !motion.exit_slides) return out;

    const float eased = ease(entering ? motion.in_easing : motion.out_easing,
                             std::clamp(t, 0.0f, 1.0f));
    // Entering runs from fully displaced to in place; leaving runs the other way. One
    // expression rather than two branches, so the two can never drift apart.
    const float displacement = entering ? 1.0f - eased : eased;
    if (displacement <= 0.0f) return out;

    if (motion.kind == MotionKind::Scale) {
        out.scale = motion.scale_from + (1.0f - motion.scale_from) * (1.0f - displacement);
        return out;
    }

    SlideAxis axis = slide_axis_for(anchor);
    if (motion.kind == MotionKind::SlideHorizontal) {
        axis = axis == SlideAxis::FromLeft ? SlideAxis::FromLeft : SlideAxis::FromRight;
    } else if (motion.kind == MotionKind::SlideVertical) {
        axis = axis == SlideAxis::FromBottom ? SlideAxis::FromBottom : SlideAxis::FromTop;
    }

    const bool horizontal = axis == SlideAxis::FromLeft || axis == SlideAxis::FromRight;
    // Zero means "my own extent along this axis", which is what puts the box fully outside the
    // edge at the start of the slide rather than merely nudged towards it.
    const float extent = horizontal ? box.w : box.h;
    const float distance = motion.distance > 0.0f ? motion.distance : extent;
    const float travel = distance * displacement;

    switch (axis) {
        case SlideAxis::FromLeft: out.dx = -travel; break;
        case SlideAxis::FromRight: out.dx = travel; break;
        case SlideAxis::FromTop: out.dy = -travel; break;
        case SlideAxis::FromBottom: out.dy = travel; break;
    }
    return out;
}

ToastFrame toast_frame(const NotificationsConfig& nc, const Viewport& viewport, float scale,
                       float stack_height) noexcept {
    ToastFrame frame;
    frame.screen_cap = std::max(96.0f, viewport.width - 16.0f);

    // The anchored edge does not depend on the box width -- a right anchor pins the right edge
    // -- which is what makes it safe to measure the room available before the boxes exist.
    const Rect reference = resolve_placement(nc.placement, frame.screen_cap, 0.0f, viewport);
    if (nc.placement.align == Align::Right) {
        frame.grow_room = reference.right() - 8.0f;
    } else if (nc.placement.align == Align::Left) {
        frame.grow_room = viewport.width - reference.x - 8.0f;
    } else {
        const float centre = reference.x + reference.w * 0.5f;
        frame.grow_room = std::min(centre, viewport.width - centre) * 2.0f - 8.0f;
    }
    frame.grow_room = std::max(96.0f, std::min(frame.grow_room, frame.screen_cap));

    // A wrapping toast still wraps at the configured width, but never wider than there is room.
    frame.wrap_cap = std::max(96.0f, std::min(nc.box.max_width * scale, frame.grow_room));

    frame.area = resolve_placement(nc.placement, frame.screen_cap, stack_height, viewport);
    return frame;
}

float toast_x(const ToastFrame& frame, Align align, float box_w,
              const Viewport& viewport) noexcept {
    float x = frame.area.x;
    if (align == Align::Right) {
        x = frame.area.right() - box_w;
    } else if (align == Align::Center) {
        x = frame.area.x + (frame.area.w - box_w) * 0.5f;
    }

    // The last resort. Everything above keeps a toast inside the viewport for any sane
    // configuration; this is what stops an insane one -- a box wider than the screen, an offset
    // dragged off the edge -- producing a toast nobody can see.
    const float rightmost = viewport.width - box_w;
    if (rightmost < 0.0f) return 0.0f;   // wider than the screen: show its left edge
    return std::clamp(x, 0.0f, rightmost);
}

FittedText fit_text(std::string_view text, float max_width, float font_size, OverflowMode mode,
                    float min_font_scale, const MeasureFn& measure, float time_s) {
    FittedText out;
    out.text = std::string(text);
    out.lines.assign(1, out.text);
    if (text.empty() || font_size <= 0.0f || !measure) return out;

    out.width = measure(text, font_size);
    if (max_width <= 0.0f || out.width <= max_width) return out;

    switch (mode) {
        case OverflowMode::Clip:
            // Nothing is altered: the renderer is expected to clip. Reporting the real width
            // rather than the budget is what lets it know it has to.
            out.truncated = true;
            return out;

        case OverflowMode::Ellipsis: {
            const float ellipsis_w = measure(kEllipsis, font_size);
            const float budget = max_width - ellipsis_w;
            if (budget <= 0.0f) {
                // Not even room for the ellipsis. An empty string is more honest than three
                // dots that overflow anyway.
                out.text.clear();
                out.lines.assign(1, out.text);
                out.width = 0.0f;
                out.truncated = true;
                return out;
            }
            std::size_t keep = 0;
            for (std::size_t i = 0; i < text.size();) {
                const std::size_t next = next_utf8(text, i);
                if (measure(text.substr(0, next), font_size) > budget) break;
                keep = next;
                i = next;
            }
            out.text = std::string(text.substr(0, keep)) + kEllipsis;
            out.lines.assign(1, out.text);
            out.width = measure(out.text, font_size);
            out.truncated = true;
            return out;
        }

        case OverflowMode::Shrink: {
            const float needed = max_width / out.width;
            out.font_scale = std::max(min_font_scale, needed);
            out.width = measure(text, font_size * out.font_scale);
            // A floor that still does not fit is a truncation the caller has to know about,
            // even though nothing was cut here.
            out.truncated = out.width > max_width;
            return out;
        }

        case OverflowMode::Scroll: {
            const float overflow = out.width - max_width;
            // A ping-pong rather than a loop: a file name that wraps around to its own start
            // is harder to read than one that slides back.
            const float period = 2.0f * (overflow / std::max(20.0f, font_size * 2.0f)) + 2.0f;
            const float phase = period > 0.0f ? std::fmod(std::max(0.0f, time_s), period) / period
                                              : 0.0f;
            const float triangle = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
            out.scroll_offset = overflow * triangle;
            out.truncated = true;
            return out;
        }

        case OverflowMode::Wrap: {
            out.lines.clear();
            std::string line;
            std::size_t word_start = 0;
            const auto flush = [&]() {
                out.lines.push_back(line);
                line.clear();
            };
            for (std::size_t i = 0; i <= text.size(); ++i) {
                const bool at_end = i == text.size();
                if (!at_end && text[i] != ' ') continue;

                const std::string_view word = text.substr(word_start, i - word_start);
                const std::string candidate = line.empty() ? std::string(word)
                                                           : line + " " + std::string(word);
                if (!line.empty() && measure(candidate, font_size) > max_width) {
                    flush();
                    line = std::string(word);
                } else {
                    line = candidate;
                }

                // A single word wider than the budget -- a long path with no spaces is the
                // common case -- is broken at the character it stops fitting at, because the
                // alternative is one line that runs off the screen.
                while (measure(line, font_size) > max_width && line.size() > 1) {
                    std::size_t keep = 0;
                    for (std::size_t j = 0; j < line.size();) {
                        const std::size_t next = next_utf8(line, j);
                        if (measure(std::string_view(line).substr(0, next), font_size) > max_width) {
                            break;
                        }
                        keep = next;
                        j = next;
                    }
                    if (keep == 0) keep = next_utf8(line, 0);
                    std::string rest = line.substr(keep);
                    line.resize(keep);
                    flush();
                    line = std::move(rest);
                }

                word_start = i + 1;
            }
            if (!line.empty()) out.lines.push_back(line);
            if (out.lines.empty()) out.lines.assign(1, std::string());

            out.width = 0.0f;
            for (const std::string& l : out.lines) {
                out.width = std::max(out.width, measure(l, font_size));
            }
            out.text = std::string(text);
            out.truncated = out.lines.size() > 1;
            return out;
        }
    }
    return out;
}

std::string format_template(std::string_view tmpl, const FormatValues& v) {
    std::vector<FormatSpan> ignored;
    return format_template(tmpl, v, ignored);
}

std::string format_template(std::string_view tmpl, const FormatValues& v,
                            std::vector<FormatSpan>& spans) {
    spans.clear();
    std::string out;
    out.reserve(tmpl.size() + 32);

    std::size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] != '{') {
            out.push_back(tmpl[i]);
            ++i;
            continue;
        }
        const std::size_t close = tmpl.find('}', i + 1);
        if (close == std::string_view::npos) {
            // An unterminated brace is literal text. Swallowing the rest of the line because a
            // user forgot a '}' would hide the mistake instead of showing it.
            out.append(tmpl.substr(i));
            break;
        }
        const std::string_view name = tmpl.substr(i + 1, close - i - 1);
        const Placeholder* placeholder = find_placeholder(name);
        if (placeholder == nullptr) {
            out.append(tmpl.substr(i, close - i + 1));
            i = close + 1;
            continue;
        }

        const std::string& value = v.*(placeholder->member);
        if (value.empty()) {
            // An absent value takes one adjacent space with it, so "Saved {file}" becomes
            // "Saved" rather than "Saved ", and "{file} was saved" becomes "was saved" rather
            // than starting with a space. The preceding space is preferred; only when there is
            // none is the following one absorbed instead. Just the one space, and only a
            // space: any other literal text is the user's and stays.
            std::size_t resume = close + 1;
            if (!out.empty() && out.back() == ' ') {
                out.pop_back();
            } else if (resume < tmpl.size() && tmpl[resume] == ' ') {
                ++resume;
            }
            i = resume;
            continue;
        }

        FormatSpan span;
        span.begin = out.size();
        out.append(value);
        span.end = out.size();
        span.field = placeholder->field;
        spans.push_back(span);
        i = close + 1;
    }
    return out;
}

FormatValues values_for(const ObsEvent& event, const ObsState& state, std::int64_t now_ms) {
    FormatValues v;
    if (!event.path.empty()) {
        v.file = file_name_of(event.path);
        v.path = event.path;
        v.folder = folder_of(event.path);
    }
    if (event.duration_ms >= 0) v.duration = format_duration(event.duration_ms);
    if (event.size_bytes >= 0) v.size = format_size(event.size_bytes);
    v.scene = event.scene.empty() ? state.current_scene : event.scene;
    v.previous_scene = event.previous_scene;
    v.profile = event.profile.empty() ? state.profile : event.profile;
    v.service = event.service.empty() ? state.stream.service : event.service;
    if (event.reason != StopReason::Unknown) v.reason = to_string(event.reason);
    v.detail = event.detail;
    if (event.attempt > 0) v.attempt = to_str(event.attempt);
    if (event.replay_seconds > 0) v.replay_seconds = to_str(event.replay_seconds);

    // {elapsed} means "how long has the output this event belongs to been running", which is a
    // different question per output and is why it is resolved here rather than by the producer.
    switch (output_of(event.kind)) {
        case OutputKind::Recording:
            if (state.recording.active()) v.elapsed = format_duration(state.recording.elapsed_ms(now_ms));
            break;
        case OutputKind::Stream:
            if (state.stream.state != OutputState::Idle) {
                v.elapsed = format_duration(state.stream.elapsed_ms(now_ms));
            }
            break;
        case OutputKind::ReplayBuffer:
            if (state.replay.active() && state.replay.started_ms > 0) {
                v.elapsed = format_duration(now_ms - state.replay.started_ms);
            }
            break;
        default:
            break;
    }

    // {duration} on a stop event falls back to how long the output had been running, because
    // that is the number the user is actually asking for and OBS does not always send it.
    if (v.duration.empty() && !v.elapsed.empty()) v.duration = v.elapsed;
    return v;
}

StatusLine status_line(const ObsState& state, const Config& config, std::int64_t now_ms) {
    const StatusConfig& sc = config.status;
    StatusLine line;
    if (!sc.placement.visible) return line;
    if (!state.obs_running && !config.general.show_when_obs_closed) return line;

    // Precedence is by how much the user stands to lose if it is wrong. A recording in progress
    // outranks a stream, which outranks an armed buffer, because a lost recording is gone and a
    // dropped stream can be restarted.
    if (sc.show_while_recording && state.recording.active()) {
        line.visible = true;
        line.paused = state.recording.state == OutputState::Paused;
        line.dot_color = line.paused ? sc.paused_color : sc.recording_color;
        if (sc.show_timer) line.text = format_duration(state.recording.elapsed_ms(now_ms));
        // A paused recording holds its light steady: a blinking dot beside a timer that is not
        // moving says the opposite of what is happening.
        line.pulsing = sc.pulse && !line.paused;
        return line;
    }
    if (sc.show_while_streaming && state.stream.active()) {
        line.visible = true;
        line.dot_color = sc.streaming_color;
        if (sc.show_timer) line.text = format_duration(state.stream.elapsed_ms(now_ms));
        line.pulsing = sc.pulse;
        return line;
    }
    if (sc.show_while_replay_armed && state.replay.active()) {
        line.visible = true;
        line.dot_color = config.appearance.accent;
        // No timer: the buffer has no start the user cares about, only a length, and showing
        // how long it has been armed would look like a recording that is being kept.
        line.pulsing = false;
        return line;
    }
    return line;
}

}  // namespace obsn
