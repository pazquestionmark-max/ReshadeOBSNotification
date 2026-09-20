// SPDX-License-Identifier: MIT
#include "obsn/notifications.hpp"
#include "obsn_test.hpp"

using namespace obsn;

namespace {

ObsEvent event_of(EventKind kind, std::int64_t ts = 1'000) {
    ObsEvent e;
    e.kind = kind;
    e.ts = ts;
    return e;
}

ObsState live_state() {
    ObsState s;
    s.obs_running = true;
    return s;
}

}  // namespace

TEST(notifications, a_recording_start_raises_the_shipped_toast) {
    Config config = Config::defaults();
    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 1'000);

    CHECK_EQ(queue.size(), std::size_t{1});
    const Notification& n = queue.items().front();
    CHECK_EQ(n.title.text, std::string("Recording started"));
    CHECK(n.detail.text.empty());
    CHECK(n.icon == IconShape::Record);
    CHECK(n.phase == NotificationPhase::Entering);
}

TEST(notifications, a_saved_recording_names_the_file_in_its_detail_line) {
    Config config = Config::defaults();
    NotificationQueue queue;
    ObsEvent event = event_of(EventKind::RecordingSaved);
    event.path = "C:\\Users\\You\\Videos\\2026-09-20 21-14-03.mkv";
    queue.submit(event, config, live_state(), 1'000);

    const Notification& n = queue.items().front();
    CHECK_EQ(n.title.text, std::string("Recording saved"));
    CHECK_EQ(n.detail.text, std::string("2026-09-20 21-14-03.mkv"));
    // The file name is a placeholder expansion, so it is highlighted.
    CHECK_EQ(n.detail.highlights.size(), std::size_t{1});
}

TEST(notifications, a_disabled_category_raises_nothing) {
    Config config = Config::defaults();
    config.notifications.recording_started.enabled = false;
    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 1'000);
    CHECK_EQ(queue.size(), std::size_t{0});
}

TEST(notifications, transition_kinds_raise_nothing_at_all) {
    // "starting" and "stopping" exist so the status indicator can show them. As toasts they
    // would be noise immediately followed by the real event.
    Config config = Config::defaults();
    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarting), config, live_state(), 1'000);
    queue.submit(event_of(EventKind::RecordingStopping), config, live_state(), 1'000);
    queue.submit(event_of(EventKind::ReplayBufferStarting), config, live_state(), 1'000);
    CHECK_EQ(queue.size(), std::size_t{0});
}

TEST(notifications, scene_changes_and_link_toasts_are_off_by_default) {
    const Config config = Config::defaults();
    CHECK(!config.notifications.scene_changed.enabled);
    CHECK(!config.notifications.obs_connected.enabled);
    CHECK(!config.notifications.obs_disconnected.enabled);
}

TEST(notifications, the_lifecycle_runs_in_order_and_ends_dead) {
    Config config = Config::defaults();
    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 0);

    const Notification& n = queue.items().front();
    const Fade fade = n.fade;

    queue.tick(fade.in_ms / 2, config);
    CHECK(queue.items().front().phase == NotificationPhase::Entering);

    queue.tick(fade.in_ms + 10, config);
    CHECK(queue.items().front().phase == NotificationPhase::Visible);

    queue.tick(fade.in_ms + fade.hold_ms + 10, config);
    CHECK(queue.items().front().phase == NotificationPhase::Leaving);

    // Removal only once the exit has finished: never mid-animation.
    queue.tick(fade.in_ms + fade.hold_ms + fade.out_ms - 1, config);
    CHECK_EQ(queue.size(), std::size_t{1});
    queue.tick(fade.in_ms + fade.hold_ms + fade.out_ms, config);
    CHECK_EQ(queue.size(), std::size_t{0});
}

TEST(notifications, opacity_follows_the_fade_envelope) {
    Config config = Config::defaults();
    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 0);
    const Notification& n = queue.items().front();

    CHECK_NEAR(n.opacity(0), 0.0f, 0.001f);
    CHECK_NEAR(n.opacity(n.fade.in_ms), 1.0f, 0.001f);
    CHECK_NEAR(n.opacity(n.fade.in_ms + n.fade.hold_ms / 2), 1.0f, 0.001f);
    CHECK_NEAR(n.opacity(n.lifetime_ms()), 0.0f, 0.001f);

    // Monotonic on the way out: a toast that brightens as it leaves reads as a flicker.
    float previous = 1.0f;
    for (int step = 0; step <= 20; ++step) {
        const std::int64_t at = n.fade.in_ms + n.fade.hold_ms +
                                (n.fade.out_ms * step) / 20;
        const float value = n.opacity(at);
        CHECK(value <= previous + 1e-4f);
        previous = value;
    }
}

TEST(notifications, identical_toasts_merge_rather_than_stacking) {
    Config config = Config::defaults();
    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 0);
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 600);

    CHECK_EQ(queue.size(), std::size_t{1});
    CHECK_EQ(queue.items().front().repeat_count, 2);
    // The lifecycle restarts, so the second occurrence is visible for its full hold.
    CHECK_EQ(queue.items().front().created_ms, 600);
}

TEST(notifications, merging_can_be_switched_off) {
    Config config = Config::defaults();
    config.notifications.merge_duplicates = false;
    config.notifications.min_interval_ms = 0;
    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 0);
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 600);
    CHECK_EQ(queue.size(), std::size_t{2});
}

TEST(notifications, the_rate_limit_replaces_rather_than_stacking) {
    Config config = Config::defaults();
    config.notifications.merge_duplicates = false;
    config.notifications.min_interval_ms = 1'000;
    config.notifications.scene_changed.enabled = true;

    NotificationQueue queue;
    ObsEvent first = event_of(EventKind::SceneChanged);
    first.scene = "Starting soon";
    queue.submit(first, config, live_state(), 0);

    ObsEvent second = event_of(EventKind::SceneChanged);
    second.scene = "Gameplay";
    queue.submit(second, config, live_state(), 200);

    // One toast, showing the newer scene: six scene changes as a collection loads should cost
    // one notification, and the last one is the true one.
    CHECK_EQ(queue.size(), std::size_t{1});
    CHECK_EQ(queue.items().front().detail.text, std::string("Gameplay"));
}

TEST(notifications, the_queue_is_bounded_and_evicts_by_priority) {
    Config config = Config::defaults();
    config.notifications.max_visible = 2;
    config.notifications.min_interval_ms = 0;
    config.notifications.scene_changed.enabled = true;

    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 0);
    ObsEvent scene = event_of(EventKind::SceneChanged);
    scene.scene = "Gameplay";
    queue.submit(scene, config, live_state(), 10);

    // A warning has the highest shipped priority, so it displaces something rather than being
    // refused, and what it displaces is the lowest-priority entry.
    ObsEvent warning = event_of(EventKind::Warning);
    warning.detail = "Dropping frames";
    queue.submit(warning, config, live_state(), 20);

    CHECK_EQ(queue.size(), std::size_t{2});
    bool has_warning = false;
    for (const Notification& n : queue.items()) {
        if (n.kind == EventKind::Warning) has_warning = true;
    }
    CHECK(has_warning);
}

TEST(notifications, a_submission_never_evicts_itself) {
    // With a bound of one, every arriving toast must be the one that survives.
    Config config = Config::defaults();
    config.notifications.max_visible = 1;
    config.notifications.min_interval_ms = 0;

    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 0);
    queue.submit(event_of(EventKind::RecordingStopped), config, live_state(), 10);
    CHECK_EQ(queue.size(), std::size_t{1});
    CHECK(queue.items().front().kind == EventKind::RecordingStopped);
}

TEST(notifications, events_just_after_connecting_are_suppressed) {
    Config config = Config::defaults();
    NotificationQueue queue;
    queue.note_connected(1'000);

    // Inside the window: this is state that was already true, not news.
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 1'100);
    CHECK_EQ(queue.size(), std::size_t{0});

    // Outside it: ordinary news again.
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(),
                 1'000 + config.notifications.suppress_after_connect_ms + 1);
    CHECK_EQ(queue.size(), std::size_t{1});
}

TEST(notifications, a_warning_with_no_text_raises_nothing) {
    // The shipped warning template is "{detail}" alone, so an empty detail would otherwise
    // produce an empty box.
    Config config = Config::defaults();
    NotificationQueue queue;
    queue.submit(event_of(EventKind::Warning), config, live_state(), 1'000);
    CHECK_EQ(queue.size(), std::size_t{0});
}

TEST(notifications, a_detail_only_template_is_promoted_to_the_title) {
    Config config = Config::defaults();
    config.notifications.recording_started.title_format = "{file}";  // never filled
    config.notifications.recording_started.detail_format = "Recording started";
    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 1'000);

    CHECK_EQ(queue.size(), std::size_t{1});
    // The toast keeps its shape rather than drawing an orphan subtitle.
    CHECK_EQ(queue.items().front().title.text, std::string("Recording started"));
    CHECK(queue.items().front().detail.text.empty());
}

TEST(notifications, the_stack_position_snaps_first_and_eases_afterwards) {
    Config config = Config::defaults();
    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 0);
    Notification& n = queue.mutable_items().front();

    // A toast that has just appeared has no previous position to travel from.
    n.settle(100.0f, 16.0f, config.animation);
    CHECK_EQ(n.stack_offset, 100.0f);

    // Afterwards it eases, arriving rather than jumping.
    n.settle(0.0f, 16.0f, config.animation);
    CHECK(n.stack_offset < 100.0f);
    CHECK(n.stack_offset > 0.0f);

    for (int i = 0; i < 200; ++i) n.settle(0.0f, 16.0f, config.animation);
    CHECK_NEAR(n.stack_offset, 0.0f, 0.51f);
}

TEST(notifications, restack_animation_can_be_switched_off) {
    Config config = Config::defaults();
    config.animation.animate_restack = false;
    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 0);
    Notification& n = queue.mutable_items().front();
    n.settle(100.0f, 16.0f, config.animation);
    n.settle(0.0f, 16.0f, config.animation);
    CHECK_EQ(n.stack_offset, 0.0f);
}

TEST(notifications, turning_animation_off_disables_motion_on_new_toasts) {
    Config config = Config::defaults();
    config.animation.enabled = false;
    NotificationQueue queue;
    queue.submit(event_of(EventKind::RecordingStarted), config, live_state(), 0);
    CHECK(queue.items().front().motion.kind == MotionKind::None);
}

TEST(notifications, a_category_can_override_the_shared_motion) {
    Config config = Config::defaults();
    config.notifications.motion.kind = MotionKind::SlideFromEdge;
    config.notifications.warning.override_motion = true;
    config.notifications.warning.motion.kind = MotionKind::Scale;

    NotificationQueue queue;
    ObsEvent warning = event_of(EventKind::Warning);
    warning.detail = "Dropping frames";
    queue.submit(warning, config, live_state(), 1'000);
    CHECK(queue.items().front().motion.kind == MotionKind::Scale);
}

TEST(notifications, sounds_are_collected_and_drained_not_played_here) {
    Config config = Config::defaults();
    config.notifications.replay_saved.sound = true;
    config.notifications.replay_saved.sound_file = "clip.wav";

    NotificationQueue queue;
    ObsEvent saved = event_of(EventKind::ReplayBufferSaved);
    saved.path = "clip.mkv";
    queue.submit(saved, config, live_state(), 1'000);

    const std::vector<std::string> sounds = queue.drain_sounds();
    CHECK_EQ(sounds.size(), std::size_t{1});
    CHECK_EQ(sounds[0], std::string("clip.wav"));
    CHECK(queue.drain_sounds().empty());
}

TEST(notifications, build_notification_is_the_same_path_the_queue_uses) {
    // The settings preview goes through this, so a divergence would mean the preview shows
    // something the overlay would never draw.
    Config config = Config::defaults();
    ObsEvent event = event_of(EventKind::ReplayBufferSaved);
    event.path = "Replay.mkv";

    Notification direct;
    CHECK(build_notification(event, config, live_state(), 1'000, direct));

    NotificationQueue queue;
    queue.submit(event, config, live_state(), 1'000);
    CHECK_EQ(queue.items().front().title.text, direct.title.text);
    CHECK_EQ(queue.items().front().detail.text, direct.detail.text);
}

TEST(notifications, every_shipped_category_produces_a_drawable_toast) {
    // A category whose template cannot be filled by its own kind of event is one that would
    // silently never appear.
    const Config config = Config::defaults();
    for (const CategoryEntry& entry : categories()) {
        NotificationStyle style = config.notifications.*(entry.member);
        if (!style.enabled) continue;

        ObsEvent event = event_of(entry.kind);
        event.path = "C:\\clips\\sample.mkv";
        event.duration_ms = 61'000;
        event.size_bytes = 1024 * 1024;
        event.scene = "Gameplay";
        event.service = "Twitch";
        event.attempt = 2;
        event.detail = "Something to report";

        Notification n;
        const bool built = build_notification(event, config, live_state(), 1'000, n);
        if (!built) {
            obsn_test::fail(__FILE__, __LINE__,
                            std::string("category '") + entry.key + "' produced no toast");
        }
        if (n.title.text.empty()) {
            obsn_test::fail(__FILE__, __LINE__,
                            std::string("category '") + entry.key + "' has an empty title");
        }
    }
}
