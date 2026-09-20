// SPDX-License-Identifier: MIT
#include "obsn/config.hpp"
#include "obsn_test.hpp"

using namespace obsn;

TEST(config, defaults_round_trip_exactly) {
    const Config defaults = Config::defaults();
    ConfigDiagnostics diag;
    const Config back = Config::parse(defaults.serialise(), diag);
    CHECK(diag.issues.empty());
    // Comparing the serialisations catches a field the writer emits and the reader ignores,
    // which a field-by-field check would miss precisely for the field someone forgot.
    CHECK_EQ(back.serialise(), defaults.serialise());
}

TEST(config, the_shipped_look_is_what_it_claims_to_be) {
    const Config c = Config::defaults();
    // These are the load-bearing figures behind "the same as the reference". A change here is
    // a change to the shipped look and should be a deliberate one.
    CHECK(c.notifications.placement.anchor == Anchor::TopRight);
    CHECK(c.notifications.box.accent_style == AccentStyle::Tile);
    CHECK(c.notifications.motion.kind == MotionKind::SlideFromEdge);
    CHECK(c.notifications.motion.in_easing == Easing::EaseOutQuint);
    CHECK_EQ(c.notifications.box.corner_radius, 2.0f);
    CHECK(c.appearance.accent == kNvidiaGreen);
    CHECK(c.notifications.recording_started.title_format == std::string("Recording started"));
    CHECK(c.notifications.replay_started.title_format == std::string("Replay buffer is on"));
    CHECK(c.notifications.replay_stopped.title_format == std::string("Replay buffer is off"));
}

TEST(config, a_malformed_document_yields_defaults_and_says_so) {
    ConfigDiagnostics diag;
    const Config c = Config::parse("{ this is not json", diag);
    CHECK(diag.from_defaults);
    CHECK(!diag.issues.empty());
    CHECK(diag.issues.front().severity == ConfigIssue::Severity::Error);
    // Still usable: loading never fails.
    CHECK_EQ(c.serialise(), Config::defaults().serialise());
}

TEST(config, a_non_object_document_is_not_a_crash) {
    ConfigDiagnostics diag;
    const Config c = Config::parse("[1, 2, 3]", diag);
    CHECK(diag.from_defaults);
    CHECK_EQ(c.notifications.max_visible, Config::defaults().notifications.max_visible);
}

TEST(config, out_of_range_numbers_are_clamped_and_reported) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"appearance":{"title_size":900},"notifications":{"max_visible":9999}})",
        diag);
    CHECK_EQ(c.appearance.title_size, 96.0f);
    CHECK_EQ(c.notifications.max_visible, 32);
    CHECK(!diag.issues.empty());
}

TEST(config, a_wrong_type_keeps_the_default_and_warns) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"appearance":{"title_size":"large","accent":12}})", diag);
    CHECK_EQ(c.appearance.title_size, Config::defaults().appearance.title_size);
    CHECK(c.appearance.accent == Config::defaults().appearance.accent);
    bool warned = false;
    for (const ConfigIssue& issue : diag.issues) {
        if (issue.severity == ConfigIssue::Severity::Warning) warned = true;
    }
    CHECK(warned);
}

TEST(config, unknown_keys_survive_a_round_trip) {
    // A profile written by a newer build and opened by an older one must not lose the settings
    // the older one did not understand.
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"something_new":{"a":1},"another":"kept"})", diag);
    const std::string out = c.serialise();
    CHECK(out.find("something_new") != std::string::npos);
    CHECK(out.find("\"another\":\"kept\"") != std::string::npos);
}

TEST(config, a_newer_version_is_flagged_and_never_downgraded) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(R"({"config_version":99})", diag);
    CHECK(diag.newer_than_supported);
    CHECK_EQ(c.config_version, 99);
}

TEST(config, colours_parse_in_every_accepted_form) {
    CHECK(Color::from_hex("#f00").value() == Color(255, 0, 0, 255));
    CHECK(Color::from_hex("f00").value() == Color(255, 0, 0, 255));
    CHECK(Color::from_hex("#f008").value() == Color(255, 0, 0, 136));
    CHECK(Color::from_hex("#76B900").value() == Color(118, 185, 0, 255));
    CHECK(Color::from_hex("#76B90080").value() == Color(118, 185, 0, 128));
    CHECK(!Color::from_hex("#gg0000").has_value());
    CHECK(!Color::from_hex("#12345").has_value());
    CHECK(!Color::from_hex("").has_value());
    CHECK_EQ(Color(118, 185, 0, 255).to_hex(), std::string("#76B900FF"));
}

TEST(config, colour_alpha_scaling_and_mixing) {
    const Color green = kNvidiaGreen;
    CHECK_EQ(green.with_alpha_scale(0.5f).a, 128);
    CHECK_EQ(green.with_alpha_scale(0.0f).a, 0);
    // Out-of-range scales clamp rather than wrapping the byte round.
    CHECK_EQ(green.with_alpha_scale(2.0f).a, 255);
    CHECK_EQ(green.with_alpha_scale(-1.0f).a, 0);

    const Color black{0, 0, 0, 255};
    CHECK(green.mix(black, 0.0f) == green);
    CHECK(green.mix(black, 1.0f) == black);
}

TEST(config, abgr_packing_matches_what_the_draw_list_expects) {
    // 0xAABBGGRR. Getting this backwards swaps red and blue everywhere at once, which is easy
    // to see and easy to introduce.
    CHECK_EQ(Color(0x12, 0x34, 0x56, 0x78).to_abgr(), 0x78563412u);
}

TEST(config, style_lookup_covers_every_category_and_only_those) {
    Config c = Config::defaults();
    for (const CategoryEntry& entry : categories()) {
        CHECK(c.style_for(entry.kind) == &(c.notifications.*(entry.member)));
    }
    // A kind with no toast must return nothing rather than falling back to some other category.
    CHECK(c.style_for(EventKind::RecordingStarting) == nullptr);
    CHECK(c.style_for(EventKind::Unknown) == nullptr);
}

TEST(config, a_zero_lifetime_toast_is_repaired) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"notifications":{"recording_started":{"fade":{"in_ms":0,"hold_ms":0,"out_ms":0}}}})",
        diag);
    // A flash is not a notification.
    CHECK(c.notifications.recording_started.fade.hold_ms > 0);
}

TEST(config, max_width_is_raised_to_meet_the_minimum_width) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"notifications":{"width":600,"box":{"max_width":200}}})", diag);
    CHECK(c.notifications.box.max_width >= c.notifications.width);
}

TEST(config, percentage_placements_are_clamped_into_the_viewport) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"notifications":{"placement":{"percent":true,"x":8,"y":-9}}})",
        diag);
    CHECK(c.notifications.placement.x <= 2.0f);
    CHECK(c.notifications.placement.y >= -1.0f);
}

TEST(config, reconnect_bounds_are_kept_consistent) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"integration":{"reconnect_initial_ms":5000,"reconnect_max_ms":100}})",
        diag);
    CHECK(c.integration.reconnect_max_ms >= c.integration.reconnect_initial_ms);
}

TEST(config, every_enum_round_trips_through_its_own_name) {
    const Anchor anchors[] = {Anchor::TopLeft, Anchor::Center, Anchor::BottomRight};
    for (const Anchor a : anchors) {
        Anchor back = Anchor::TopLeft;
        CHECK(parse_enum(to_string(a), back));
        CHECK(back == a);
    }
    for (int i = 0; i <= static_cast<int>(IconShape::Disk); ++i) {
        const IconShape shape = static_cast<IconShape>(i);
        IconShape back = IconShape::None;
        CHECK(parse_enum(to_string(shape), back));
        CHECK(back == shape);
    }
    for (int i = 0; i <= static_cast<int>(Easing::EaseOutElastic); ++i) {
        const Easing easing = static_cast<Easing>(i);
        Easing back = Easing::Linear;
        CHECK(parse_enum(to_string(easing), back));
        CHECK(back == easing);
    }
    for (int i = 0; i <= static_cast<int>(MotionKind::Scale); ++i) {
        const MotionKind kind = static_cast<MotionKind>(i);
        MotionKind back = MotionKind::None;
        CHECK(parse_enum(to_string(kind), back));
        CHECK(back == kind);
    }
}

TEST(config, an_unrecognised_enum_keeps_the_default) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"notifications":{"placement":{"anchor":"somewhere_else"}}})",
        diag);
    CHECK(c.notifications.placement.anchor == Anchor::TopRight);
    CHECK(!diag.issues.empty());
}

TEST(config, floats_serialise_readably) {
    // 0.55f widened to a double is 0.550000011920928955. A configuration file full of that is
    // one nobody wants to hand-edit.
    const std::string out = Config::defaults().serialise();
    CHECK(out.find("0.550000") == std::string::npos);
    CHECK(out.find("\"border_accent_opacity\":0.55") != std::string::npos);
}

TEST(config, the_category_table_has_no_duplicates) {
    const std::vector<CategoryEntry>& table = categories();
    for (std::size_t i = 0; i < table.size(); ++i) {
        for (std::size_t j = i + 1; j < table.size(); ++j) {
            CHECK(std::string(table[i].key) != std::string(table[j].key));
            CHECK(table[i].kind != table[j].kind);
        }
    }
}
