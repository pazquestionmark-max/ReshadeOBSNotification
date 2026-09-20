// SPDX-License-Identifier: MIT
#include "obsn/notifications.hpp"

#include <algorithm>
#include <cmath>

namespace obsn {
namespace {

NotificationLine build_line(std::string_view format, const FormatValues& values,
                            const NotificationStyle& style, Color base) {
    NotificationLine line;
    line.color = base;
    if (format.empty()) return line;

    std::vector<FormatSpan> spans;
    line.text = format_template(format, values, spans);
    if (style.color_placeholders) {
        line.highlights.reserve(spans.size());
        for (const FormatSpan& span : spans) {
            if (span.end > span.begin) {
                line.highlights.push_back(NotificationLine::Highlight{span.begin, span.end});
            }
        }
    }
    return line;
}

}  // namespace

std::int64_t Notification::lifetime_ms() const noexcept {
    return static_cast<std::int64_t>(fade.in_ms) + fade.hold_ms + fade.out_ms;
}

float Notification::opacity(std::int64_t now_ms) const noexcept {
    const std::int64_t age = now_ms - created_ms;
    if (age < 0) return fade.start_opacity;

    if (age < fade.in_ms) {
        const float t = fade.in_ms > 0
                            ? static_cast<float>(age) / static_cast<float>(fade.in_ms)
                            : 1.0f;
        return fade.start_opacity + (fade.end_opacity - fade.start_opacity) * ease(fade.easing, t);
    }

    const std::int64_t visible_until = static_cast<std::int64_t>(fade.in_ms) + fade.hold_ms;
    if (age < visible_until) return fade.end_opacity;
    if (fade.out_ms <= 0) return 0.0f;

    const float t = static_cast<float>(age - visible_until) / static_cast<float>(fade.out_ms);
    if (t >= 1.0f) return 0.0f;
    // The exit eases out of the held opacity with a curve that cannot overshoot. An overshoot on
    // the way out reads as a flicker, whatever it looks like on the way in.
    return fade.end_opacity * (1.0f - ease(Easing::EaseOutCubic, t));
}

float Notification::animation_progress(std::int64_t now_ms, bool& entering) const noexcept {
    const std::int64_t age = now_ms - created_ms;
    entering = true;
    if (age < 0) return 0.0f;
    if (age < fade.in_ms) {
        return fade.in_ms > 0 ? static_cast<float>(age) / static_cast<float>(fade.in_ms) : 1.0f;
    }
    const std::int64_t visible_until = static_cast<std::int64_t>(fade.in_ms) + fade.hold_ms;
    if (age < visible_until) return 1.0f;

    entering = false;
    if (fade.out_ms <= 0) return 1.0f;
    return std::min(1.0f, static_cast<float>(age - visible_until) /
                              static_cast<float>(fade.out_ms));
}

bool Notification::finished(std::int64_t now_ms) const noexcept {
    return now_ms - created_ms >= lifetime_ms();
}

void Notification::settle(float target, float dt_ms, const AnimationConfig& animation) noexcept {
    if (!stack_offset_valid) {
        stack_offset = target;
        stack_offset_valid = true;
        return;
    }
    if (!animation.enabled || !animation.animate_restack || animation.restack_ms <= 0) {
        stack_offset = target;
        return;
    }
    if (dt_ms <= 0.0f) return;

    const float delta = target - stack_offset;
    if (std::fabs(delta) < 0.5f) {
        stack_offset = target;
        return;
    }
    // A fixed fraction per millisecond, shaped by the easing curve. This is frame-rate
    // independent by construction: the same wall-clock interval moves the same distance whether
    // it arrived as one long frame or six short ones.
    const float step = std::min(1.0f, dt_ms / static_cast<float>(animation.restack_ms));
    stack_offset += delta * ease(animation.restack_easing, step);
}

bool build_notification(const ObsEvent& event, const Config& config, const ObsState& state,
                        std::int64_t now_ms, Notification& out) {
    const NotificationStyle* style = config.style_for(event.kind);
    if (style == nullptr || !style->enabled) return false;

    const FormatValues values = values_for(event, state, now_ms);

    out = Notification{};
    out.kind = event.kind;
    out.accent = style->accent;
    out.icon = style->icon;
    out.placeholder_color = style->placeholder_color;
    out.title = build_line(style->title_format, values, *style,
                           style->override_title_color ? style->title_color
                                                       : config.appearance.title_color);
    out.detail = build_line(style->detail_format, values, *style,
                            style->override_detail_color ? style->detail_color
                                                         : config.appearance.detail_color);
    out.wrap = style->wrap;
    out.max_lines = style->max_lines;
    out.priority = style->priority;
    out.created_ms = now_ms;
    out.fade = style->fade;
    out.motion = style->override_motion ? style->motion : config.notifications.motion;
    if (!config.animation.enabled) out.motion.kind = MotionKind::None;
    out.phase = NotificationPhase::Entering;
    out.play_sound = style->sound && !style->sound_file.empty();
    out.sound_file = style->sound_file;

    // A title that expanded to nothing is not a notification. This happens when the only thing
    // a template had to say was a placeholder the producer could not fill -- a warning with no
    // text, say -- and an empty box on screen is worse than no box.
    if (out.title.text.empty() && out.detail.text.empty()) return false;
    // A detail line with no title is promoted rather than drawn as an orphan subtitle, so the
    // toast keeps the reference's shape whichever of the two templates was filled.
    if (out.title.text.empty()) {
        out.title = out.detail;
        out.title.color = style->override_title_color ? style->title_color
                                                      : config.appearance.title_color;
        out.detail = NotificationLine{};
    }
    return true;
}

void NotificationQueue::clear() {
    items_.clear();
    last_shown_.clear();
}

void NotificationQueue::submit(const ObsEvent& event, const Config& config,
                               const ObsState& state, std::int64_t now_ms) {
    const NotificationsConfig& nc = config.notifications;

    Notification n;
    if (!build_notification(event, config, state, now_ms, n)) return;

    // Post-connect suppression. A snapshot arriving after a reconnect describes what is already
    // true; announcing it would tell the user their recording just started when it started ten
    // minutes ago.
    if (connected_at_ms_ > 0 && now_ms - connected_at_ms_ < nc.suppress_after_connect_ms &&
        event.kind != EventKind::ScriptConnected) {
        return;
    }

    // Merge an identical toast that is still on its way in or sitting there, rather than
    // stacking two copies of the same sentence.
    if (nc.merge_duplicates) {
        for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
            if (it->kind != n.kind || it->phase == NotificationPhase::Leaving) continue;
            if (it->title.text != n.title.text || it->detail.text != n.detail.text) continue;
            ++it->repeat_count;
            it->created_ms = now_ms;
            it->phase = NotificationPhase::Visible;
            return;
        }
    }

    // Per-category rate limit. Two different events from one category arriving inside the
    // window replace each other in place instead of stacking: OBS emitting six scene changes
    // as a collection loads should cost one toast, not six, and the last one is the true one.
    auto last = std::find_if(last_shown_.begin(), last_shown_.end(),
                             [&](const std::pair<EventKind, std::int64_t>& e) {
                                 return e.first == n.kind;
                             });
    const bool rate_limited = nc.min_interval_ms > 0 && last != last_shown_.end() &&
                              now_ms - last->second < nc.min_interval_ms;
    if (rate_limited) {
        for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
            if (it->kind != n.kind || it->phase == NotificationPhase::Leaving) continue;
            const std::uint64_t id = it->id;
            const float offset = it->stack_offset;
            const bool offset_valid = it->stack_offset_valid;
            n.id = id;
            // The replacement inherits the slot it is taking over, so the stack does not jump
            // when the text changes underneath it.
            n.stack_offset = offset;
            n.stack_offset_valid = offset_valid;
            *it = std::move(n);
            return;
        }
    }

    if (n.play_sound) pending_sounds_.push_back(n.sound_file);
    n.id = next_id_++;
    const std::uint64_t id = n.id;
    items_.push_back(std::move(n));

    if (last != last_shown_.end()) last->second = now_ms;
    else last_shown_.emplace_back(event.kind, now_ms);

    enforce_bound(static_cast<std::size_t>(std::max(1, nc.max_visible)), id);
}

void NotificationQueue::enforce_bound(std::size_t max, std::uint64_t protect) {
    while (items_.size() > max) {
        // Lowest priority goes first; among equals, the oldest. The toast that triggered this
        // eviction is never the one evicted -- an event that arrives when the screen is full
        // should displace something, not vanish.
        auto victim = items_.end();
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->id == protect) continue;
            if (victim == items_.end() || it->priority < victim->priority ||
                (it->priority == victim->priority && it->created_ms < victim->created_ms)) {
                victim = it;
            }
        }
        if (victim == items_.end()) break;  // nothing evictable: every entry is protected
        items_.erase(victim);
    }
}

std::size_t NotificationQueue::tick(std::int64_t now_ms, const Config& config) {
    const std::size_t before = items_.size();

    for (Notification& n : items_) {
        const std::int64_t age = now_ms - n.created_ms;
        if (age < n.fade.in_ms) {
            n.phase = NotificationPhase::Entering;
        } else if (age < static_cast<std::int64_t>(n.fade.in_ms) + n.fade.hold_ms) {
            n.phase = NotificationPhase::Visible;
        } else if (age < n.lifetime_ms()) {
            n.phase = NotificationPhase::Leaving;
        } else {
            n.phase = NotificationPhase::Dead;
        }
    }

    // Only fully-finished entries are erased. An entry mid-exit keeps its slot, which is what
    // makes "never removed mid-animation" true rather than merely intended.
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](const Notification& n) {
                                    return n.phase == NotificationPhase::Dead;
                                }),
                 items_.end());

    enforce_bound(static_cast<std::size_t>(std::max(1, config.notifications.max_visible)), 0);

    // The rate-limit table is per category, so it is bounded by the number of categories and
    // needs no eviction of its own. It is trimmed only when the queue empties, so a session
    // that runs for hours does not carry stale timestamps into a new burst.
    if (items_.empty() && last_shown_.size() > categories().size()) last_shown_.clear();

    return before - items_.size();
}

std::vector<std::string> NotificationQueue::drain_sounds() {
    std::vector<std::string> out;
    out.swap(pending_sounds_);
    return out;
}

}  // namespace obsn
