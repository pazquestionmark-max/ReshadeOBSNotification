// SPDX-License-Identifier: MIT
// Notification lifecycle: turning OBS events into toasts, and ageing them out.
//
// The lifecycle is explicit and total: created -> entering -> visible -> leaving -> dead. A
// notification is never removed mid-animation; expiry marks it for the exit and removal happens
// only once that exit has finished. Nothing here draws, measures or reads a clock of its own --
// `now_ms` is always passed in, which is what makes every timing rule below a unit test rather
// than something you have to sit and watch.
#ifndef OBSN_NOTIFICATIONS_HPP
#define OBSN_NOTIFICATIONS_HPP

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "obsn/config.hpp"
#include "obsn/layout.hpp"
#include "obsn/model.hpp"

namespace obsn {

enum class NotificationPhase { Entering, Visible, Leaving, Dead };

/// One line of a toast, already expanded and ready to draw.
struct NotificationLine {
    std::string text;
    Color color{};
    /// Runs of `text` that came from a placeholder. The renderer draws these in
    /// `placeholder_color` and everything between them in `color`.
    struct Highlight {
        std::size_t begin = 0;
        std::size_t end = 0;
    };
    std::vector<Highlight> highlights;
};

struct Notification {
    std::uint64_t id = 0;
    EventKind kind = EventKind::Unknown;

    NotificationLine title;
    NotificationLine detail;
    Color placeholder_color{};
    Color accent{};
    IconShape icon = IconShape::None;

    bool wrap = false;
    int max_lines = 2;
    int priority = 0;
    bool play_sound = false;
    std::string sound_file;

    std::int64_t created_ms = 0;
    Fade fade;
    Motion motion;
    NotificationPhase phase = NotificationPhase::Entering;
    int repeat_count = 1;   ///< > 1 when merge_duplicates folded identical events together

    /// Animated position within the stack, in pixels from the stack's origin. Held here rather
    /// than in the renderer so it survives the per-frame layout being thrown away, which is what
    /// lets the stack close up smoothly when a toast above it expires.
    float stack_offset = 0.0f;
    bool stack_offset_valid = false;

    /// Current opacity, 0..1.
    float opacity(std::int64_t now_ms) const noexcept;
    /// How far through the entry (or exit) animation this is, 0..1, and which one it is.
    /// Returns 1 with `entering = true` while the toast is simply sitting there.
    float animation_progress(std::int64_t now_ms, bool& entering) const noexcept;
    /// Whether the whole lifecycle has completed and the entry can be dropped.
    bool finished(std::int64_t now_ms) const noexcept;
    std::int64_t lifetime_ms() const noexcept;

    /// Eases `stack_offset` towards `target`. The first call snaps, because a toast that has
    /// just appeared has no previous position to travel from.
    void settle(float target, float dt_ms, const AnimationConfig& animation) noexcept;
};

/// Turns ObsEvents into Notifications and ages them out.
///
/// Bounded by config.max_visible; overflow evicts by priority and then by age, so a
/// dropped-frames warning outlives a scene change rather than the other way round.
class NotificationQueue {
public:
    /// Seeds the post-connect suppression window, so a snapshot arriving after a reconnect does
    /// not announce state that was already true.
    void note_connected(std::int64_t now_ms) { connected_at_ms_ = now_ms; }

    /// Submits one event. `state` is the world it happened against, used to resolve {elapsed}
    /// and to fill in values the producer left out.
    void submit(const ObsEvent& event, const Config& config, const ObsState& state,
                std::int64_t now_ms);

    /// Advances lifecycles and removes finished entries. Returns the number removed.
    std::size_t tick(std::int64_t now_ms, const Config& config);
    void clear();

    const std::deque<Notification>& items() const noexcept { return items_; }
    std::deque<Notification>& mutable_items() noexcept { return items_; }
    std::size_t size() const noexcept { return items_.size(); }

    /// Sounds requested since the last drain. The renderer plays and clears these; the queue
    /// itself never touches audio.
    std::vector<std::string> drain_sounds();

private:
    /// Applies the overflow bound. Never evicts `protect`, so a toast cannot be dropped by the
    /// very submission that created it.
    void enforce_bound(std::size_t max, std::uint64_t protect);

    std::deque<Notification> items_;
    std::vector<std::string> pending_sounds_;
    std::uint64_t next_id_ = 1;
    std::int64_t connected_at_ms_ = 0;
    /// When each category last raised a toast, for the per-category rate limit.
    std::vector<std::pair<EventKind, std::int64_t>> last_shown_;
};

/// Builds a notification from an event without queueing it. Exposed so the settings preview and
/// the tests exercise the real formatting path rather than a parallel mock of it.
/// Returns false when the category is disabled or the kind raises no toast.
bool build_notification(const ObsEvent& event, const Config& config, const ObsState& state,
                        std::int64_t now_ms, Notification& out);

}  // namespace obsn

#endif  // OBSN_NOTIFICATIONS_HPP
