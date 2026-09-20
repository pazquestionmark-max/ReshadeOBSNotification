// SPDX-License-Identifier: MIT
#include "obsn/layout.hpp"
#include "obsn_test.hpp"

using namespace obsn;

namespace {

/// A deterministic stand-in for a real font: every codepoint is 0.5em wide. That is enough to
/// exercise every fitting rule, and it makes the expected numbers in these tests arithmetic
/// rather than whatever a particular .ttf happens to measure.
MeasureFn stub_measure() {
    return [](std::string_view text, float size) {
        std::size_t glyphs = 0;
        for (const char c : text) {
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++glyphs;
        }
        return static_cast<float>(glyphs) * size * 0.5f;
    };
}

Viewport hd() { return Viewport{1920.0f, 1080.0f}; }

}  // namespace

TEST(layout, top_right_placement_keeps_its_margin_at_any_resolution) {
    Placement p;
    p.anchor = Anchor::TopRight;
    p.x = 32.0f;
    p.y = 32.0f;

    const Rect hd_rect = resolve_placement(p, 320.0f, 56.0f, Viewport{1920.0f, 1080.0f});
    CHECK_EQ(hd_rect.y, 32.0f);
    CHECK_EQ(1920.0f - hd_rect.right(), 32.0f);

    const Rect uhd = resolve_placement(p, 320.0f, 56.0f, Viewport{3840.0f, 2160.0f});
    CHECK_EQ(uhd.y, 32.0f);
    CHECK_EQ(3840.0f - uhd.right(), 32.0f);

    // An ultrawide is the case that catches an anchor implemented as a fraction by mistake.
    const Rect ultrawide = resolve_placement(p, 320.0f, 56.0f, Viewport{5120.0f, 1440.0f});
    CHECK_EQ(5120.0f - ultrawide.right(), 32.0f);
}

TEST(layout, bottom_anchors_measure_from_the_bottom) {
    Placement p;
    p.anchor = Anchor::BottomRight;
    p.x = 24.0f;
    p.y = 24.0f;
    const Rect r = resolve_placement(p, 200.0f, 60.0f, hd());
    CHECK_EQ(1080.0f - r.bottom(), 24.0f);
    CHECK_EQ(1920.0f - r.right(), 24.0f);
}

TEST(layout, percentage_offsets_scale_with_the_viewport) {
    Placement p;
    p.anchor = Anchor::TopLeft;
    p.percent = true;
    p.x = 0.1f;
    p.y = 0.25f;
    const Rect r = resolve_placement(p, 100.0f, 50.0f, hd());
    CHECK_NEAR(r.x, 192.0f, 0.01f);
    CHECK_NEAR(r.y, 270.0f, 0.01f);
}

TEST(layout, a_centred_anchor_centres_then_applies_the_offset) {
    Placement p;
    p.anchor = Anchor::Center;
    p.x = 0.0f;
    p.y = 0.0f;
    const Rect centred = resolve_placement(p, 300.0f, 100.0f, hd());
    CHECK_NEAR(centred.x, (1920.0f - 300.0f) * 0.5f, 0.01f);
    CHECK_NEAR(centred.y, (1080.0f - 100.0f) * 0.5f, 0.01f);

    // A non-zero offset nudges it rather than being ignored, which is the bug this catches.
    p.x = 40.0f;
    CHECK_NEAR(resolve_placement(p, 300.0f, 100.0f, hd()).x,
               (1920.0f - 300.0f) * 0.5f + 40.0f, 0.01f);
}

TEST(layout, ellipsis_never_exceeds_its_budget) {
    const MeasureFn measure = stub_measure();
    const std::string text = "2026-09-20 21-14-03 Gameplay Capture.mkv";
    const FittedText fitted = fit_text(text, 100.0f, 14.0f, OverflowMode::Ellipsis, 0.75f,
                                       measure);
    CHECK(fitted.truncated);
    CHECK(fitted.width <= 100.0f);
    CHECK(fitted.text.size() >= 3);
    CHECK_EQ(fitted.text.substr(fitted.text.size() - 3), std::string("..."));
}

TEST(layout, a_budget_too_small_even_for_the_ellipsis_yields_nothing) {
    // Three dots that overflow anyway are worse than no text: the box would be wrong either
    // way, and only one of the two admits it.
    const FittedText fitted = fit_text("something long", 4.0f, 14.0f, OverflowMode::Ellipsis,
                                       0.75f, stub_measure());
    CHECK(fitted.text.empty());
    CHECK(fitted.truncated);
}

TEST(layout, text_that_fits_is_left_exactly_alone) {
    const FittedText fitted = fit_text("Recording started", 400.0f, 15.0f,
                                       OverflowMode::Ellipsis, 0.75f, stub_measure());
    CHECK(!fitted.truncated);
    CHECK_EQ(fitted.text, std::string("Recording started"));
    CHECK_EQ(fitted.lines.size(), std::size_t{1});
}

TEST(layout, wrap_breaks_on_spaces_and_every_line_fits) {
    const MeasureFn measure = stub_measure();
    const std::string text = "Encoder overloaded: frames are being skipped by the encoder";
    const FittedText fitted = fit_text(text, 120.0f, 13.0f, OverflowMode::Wrap, 1.0f, measure);
    CHECK(fitted.lines.size() > 1);
    for (const std::string& line : fitted.lines) {
        CHECK(measure(line, 13.0f) <= 120.0f + 0.01f);
    }
}

TEST(layout, wrap_breaks_a_single_over_long_word) {
    // A long path with no spaces is the ordinary case here, and the one a naive word-wrapper
    // turns into one line running off the screen.
    const MeasureFn measure = stub_measure();
    const std::string path = "C:\\Users\\Somebody\\Videos\\2026-09-20-21-14-03-gameplay.mkv";
    const FittedText fitted = fit_text(path, 80.0f, 13.0f, OverflowMode::Wrap, 1.0f, measure);
    CHECK(fitted.lines.size() > 1);
    for (const std::string& line : fitted.lines) {
        CHECK(measure(line, 13.0f) <= 80.0f + 0.01f);
    }
}

TEST(layout, shrink_respects_its_floor) {
    const FittedText fitted = fit_text("a very long line indeed", 20.0f, 16.0f,
                                       OverflowMode::Shrink, 0.75f, stub_measure());
    CHECK_NEAR(fitted.font_scale, 0.75f, 0.001f);
    // The floor was reached and it still does not fit, which the caller has to be told.
    CHECK(fitted.truncated);
}

TEST(layout, placeholders_expand) {
    FormatValues v;
    v.file = "clip.mkv";
    v.duration = "12:07";
    CHECK_EQ(format_template("Recording saved", v), std::string("Recording saved"));
    CHECK_EQ(format_template("{file}", v), std::string("clip.mkv"));
    CHECK_EQ(format_template("Saved {file} ({duration})", v),
             std::string("Saved clip.mkv (12:07)"));
}

TEST(layout, an_unknown_placeholder_is_left_visible) {
    FormatValues v;
    // Silently swallowing a typo means the user sees a gap and has nothing to correct.
    CHECK_EQ(format_template("Saved {filename}", v), std::string("Saved {filename}"));
    CHECK_EQ(format_template("Unclosed {file", v), std::string("Unclosed {file"));
}

TEST(layout, an_empty_value_takes_one_adjacent_space_with_it) {
    FormatValues v;   // every value empty
    CHECK_EQ(format_template("Recording saved {file}", v), std::string("Recording saved"));
    CHECK_EQ(format_template("{file} was saved", v), std::string("was saved"));
    // Only one space is absorbed, and only spaces: other literal text is the user's own.
    CHECK_EQ(format_template("Saved: {file}", v), std::string("Saved:"));
}

TEST(layout, spans_record_where_each_value_landed) {
    FormatValues v;
    v.file = "clip.mkv";
    v.duration = "12:07";
    std::vector<FormatSpan> spans;
    const std::string out = format_template("Saved {file} in {duration}", v, spans);
    CHECK_EQ(spans.size(), std::size_t{2});
    CHECK_EQ(out.substr(spans[0].begin, spans[0].end - spans[0].begin), std::string("clip.mkv"));
    CHECK(spans[0].field == FormatField::File);
    CHECK_EQ(out.substr(spans[1].begin, spans[1].end - spans[1].begin), std::string("12:07"));
    CHECK(spans[1].field == FormatField::Duration);
}

TEST(layout, values_are_built_from_the_event_and_the_state_together) {
    ObsState state;
    state.current_scene = "Gameplay";
    state.stream.service = "Twitch";
    state.recording.state = OutputState::Active;
    state.recording.started_ms = 1'000;

    ObsEvent event;
    event.kind = EventKind::RecordingSaved;
    event.path = "C:\\clips\\a.mkv";
    event.size_bytes = 1024 * 1024;

    const FormatValues v = values_for(event, state, 61'000);
    CHECK_EQ(v.file, std::string("a.mkv"));
    CHECK_EQ(v.folder, std::string("C:\\clips"));
    CHECK_EQ(v.size, std::string("1.0 MB"));
    // The scene came from the state because the event did not carry one.
    CHECK_EQ(v.scene, std::string("Gameplay"));
    CHECK_EQ(v.elapsed, std::string("1:00"));
    // {duration} falls back to the elapsed time, which is the number the user wants and which
    // OBS does not always send.
    CHECK_EQ(v.duration, std::string("1:00"));
}

TEST(layout, status_line_prefers_recording_over_streaming) {
    Config config = Config::defaults();
    config.status.placement.visible = true;

    ObsState state;
    state.obs_running = true;
    state.recording.state = OutputState::Active;
    state.recording.started_ms = 1'000;
    state.stream.state = OutputState::Active;
    state.stream.started_ms = 1'000;

    const StatusLine line = status_line(state, config, 66'000);
    CHECK(line.visible);
    CHECK_EQ(line.text, std::string("1:05"));
    CHECK(line.dot_color == config.status.recording_color);
    CHECK(line.pulsing);
}

TEST(layout, a_paused_recording_holds_its_light_steady) {
    Config config = Config::defaults();
    config.status.placement.visible = true;

    ObsState state;
    state.obs_running = true;
    state.recording.state = OutputState::Paused;
    state.recording.started_ms = 1'000;
    state.recording.paused_since_ms = 31'000;

    const StatusLine line = status_line(state, config, 66'000);
    CHECK(line.visible);
    CHECK(line.paused);
    // A blinking dot beside a timer that is not moving says the opposite of what is happening.
    CHECK(!line.pulsing);
    CHECK_EQ(line.text, std::string("0:30"));
    CHECK(line.dot_color == config.status.paused_color);
}

TEST(layout, the_status_line_is_hidden_when_nothing_is_running) {
    Config config = Config::defaults();
    config.status.placement.visible = true;
    ObsState state;
    state.obs_running = true;
    CHECK(!status_line(state, config, 1'000).visible);

    // And with OBS closed, nothing is claimed at all.
    state.recording.state = OutputState::Active;
    state.obs_running = false;
    CHECK(!status_line(state, config, 1'000).visible);
}

TEST(layout, an_armed_replay_buffer_alone_is_opt_in) {
    Config config = Config::defaults();
    config.status.placement.visible = true;
    ObsState state;
    state.obs_running = true;
    state.replay.state = OutputState::Active;

    CHECK(!status_line(state, config, 1'000).visible);
    config.status.show_while_replay_armed = true;
    const StatusLine line = status_line(state, config, 1'000);
    CHECK(line.visible);
    // No timer: the buffer has no start the user cares about, only a length.
    CHECK(line.text.empty());
}
