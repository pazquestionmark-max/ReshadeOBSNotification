// SPDX-License-Identifier: MIT
// The entry and exit animation, which is the part of "the same as the reference" that is a
// claim about behaviour over time rather than about a still frame.
#include "obsn/config.hpp"
#include "obsn/layout.hpp"
#include "obsn_test.hpp"

using namespace obsn;

namespace {
Rect box() { return Rect{100.0f, 50.0f, 320.0f, 56.0f}; }
}  // namespace

TEST(motion, easings_are_bounded_and_hit_both_endpoints) {
    const Easing all[] = {
        Easing::Linear, Easing::EaseIn, Easing::EaseOut, Easing::EaseInOut,
        Easing::EaseInCubic, Easing::EaseOutCubic, Easing::EaseInOutCubic,
        Easing::EaseOutQuint, Easing::EaseOutExpo, Easing::EaseOutBack, Easing::EaseOutElastic,
    };
    for (const Easing easing : all) {
        CHECK_EQ(ease(easing, 0.0f), 0.0f);
        CHECK_EQ(ease(easing, 1.0f), 1.0f);
        // Running past the end returns the end rather than extrapolating off the curve.
        CHECK_EQ(ease(easing, 1.5f), 1.0f);
        CHECK_EQ(ease(easing, -0.5f), 0.0f);
        for (int step = 1; step < 10; ++step) {
            const float t = static_cast<float>(step) / 10.0f;
            const float value = ease(easing, t);
            // Back and elastic deliberately overshoot; nothing may run away.
            CHECK(value > -0.5f);
            CHECK(value < 1.6f);
        }
    }
}

TEST(motion, ease_out_curves_are_monotonic) {
    // An "ease out" that dips backwards would read as a stutter mid-slide. Back and elastic are
    // excluded because overshooting is exactly what they are for.
    const Easing monotonic[] = {Easing::Linear, Easing::EaseOut, Easing::EaseOutCubic,
                                Easing::EaseOutQuint, Easing::EaseOutExpo};
    for (const Easing easing : monotonic) {
        float previous = 0.0f;
        for (int step = 1; step <= 100; ++step) {
            const float value = ease(easing, static_cast<float>(step) / 100.0f);
            CHECK(value >= previous - 1e-5f);
            previous = value;
        }
    }
}

TEST(motion, a_right_anchored_toast_slides_in_from_the_right) {
    Motion motion;
    motion.kind = MotionKind::SlideFromEdge;
    motion.distance = 0.0f;   // travel its own width

    // At the very start it is displaced by a full box width, to the right -- fully off screen.
    const MotionOffset start = motion_offset(motion, Anchor::TopRight, box(), 0.0f, true);
    CHECK_NEAR(start.dx, 320.0f, 0.01f);
    CHECK_EQ(start.dy, 0.0f);

    // At the end it is exactly in place.
    const MotionOffset end = motion_offset(motion, Anchor::TopRight, box(), 1.0f, true);
    CHECK_NEAR(end.dx, 0.0f, 0.01f);

    // And it only ever moves towards its resting place.
    float previous = start.dx;
    for (int step = 1; step <= 20; ++step) {
        const float t = static_cast<float>(step) / 20.0f;
        const float dx = motion_offset(motion, Anchor::TopRight, box(), t, true).dx;
        CHECK(dx <= previous + 1e-4f);
        previous = dx;
    }
}

TEST(motion, a_left_anchored_toast_slides_in_from_the_left) {
    Motion motion;
    motion.kind = MotionKind::SlideFromEdge;
    const MotionOffset start = motion_offset(motion, Anchor::BottomLeft, box(), 0.0f, true);
    CHECK_NEAR(start.dx, -320.0f, 0.01f);
}

TEST(motion, vertical_anchors_slide_vertically) {
    Motion motion;
    motion.kind = MotionKind::SlideFromEdge;
    CHECK_NEAR(motion_offset(motion, Anchor::TopCenter, box(), 0.0f, true).dy, -56.0f, 0.01f);
    CHECK_NEAR(motion_offset(motion, Anchor::BottomCenter, box(), 0.0f, true).dy, 56.0f, 0.01f);
    CHECK_EQ(motion_offset(motion, Anchor::TopCenter, box(), 0.0f, true).dx, 0.0f);
}

TEST(motion, an_explicit_distance_overrides_the_box_width) {
    Motion motion;
    motion.kind = MotionKind::SlideFromEdge;
    motion.distance = 40.0f;
    CHECK_NEAR(motion_offset(motion, Anchor::TopRight, box(), 0.0f, true).dx, 40.0f, 0.01f);
}

TEST(motion, the_exit_runs_the_other_way) {
    Motion motion;
    motion.kind = MotionKind::SlideFromEdge;

    // Leaving starts in place and ends displaced -- the mirror of arriving.
    CHECK_NEAR(motion_offset(motion, Anchor::TopRight, box(), 0.0f, false).dx, 0.0f, 0.01f);
    CHECK_NEAR(motion_offset(motion, Anchor::TopRight, box(), 1.0f, false).dx, 320.0f, 0.01f);
}

TEST(motion, exit_slides_can_be_switched_off_independently) {
    Motion motion;
    motion.kind = MotionKind::SlideFromEdge;
    motion.exit_slides = false;
    // Entry still slides...
    CHECK_NEAR(motion_offset(motion, Anchor::TopRight, box(), 0.0f, true).dx, 320.0f, 0.01f);
    // ...and the exit only fades.
    CHECK_EQ(motion_offset(motion, Anchor::TopRight, box(), 1.0f, false).dx, 0.0f);
}

TEST(motion, none_never_moves_anything) {
    Motion motion;
    motion.kind = MotionKind::None;
    for (int step = 0; step <= 10; ++step) {
        const float t = static_cast<float>(step) / 10.0f;
        const MotionOffset in = motion_offset(motion, Anchor::TopRight, box(), t, true);
        const MotionOffset out = motion_offset(motion, Anchor::TopRight, box(), t, false);
        CHECK_EQ(in.dx, 0.0f);
        CHECK_EQ(in.dy, 0.0f);
        CHECK_EQ(in.scale, 1.0f);
        CHECK_EQ(out.dx, 0.0f);
    }
}

TEST(motion, scale_grows_from_the_configured_start) {
    Motion motion;
    motion.kind = MotionKind::Scale;
    motion.scale_from = 0.8f;
    CHECK_NEAR(motion_offset(motion, Anchor::TopRight, box(), 0.0f, true).scale, 0.8f, 0.001f);
    CHECK_NEAR(motion_offset(motion, Anchor::TopRight, box(), 1.0f, true).scale, 1.0f, 0.001f);
    // Scaling never also translates: the two are separate settings for a reason.
    CHECK_EQ(motion_offset(motion, Anchor::TopRight, box(), 0.3f, true).dx, 0.0f);
}

TEST(motion, forced_axes_ignore_the_anchor) {
    Motion horizontal;
    horizontal.kind = MotionKind::SlideHorizontal;
    // A top-centre anchor would slide vertically under SlideFromEdge; forced horizontal must not.
    CHECK_EQ(motion_offset(horizontal, Anchor::TopCenter, box(), 0.0f, true).dy, 0.0f);
    CHECK(motion_offset(horizontal, Anchor::TopCenter, box(), 0.0f, true).dx != 0.0f);

    Motion vertical;
    vertical.kind = MotionKind::SlideVertical;
    CHECK_EQ(motion_offset(vertical, Anchor::TopRight, box(), 0.0f, true).dx, 0.0f);
    CHECK(motion_offset(vertical, Anchor::TopRight, box(), 0.0f, true).dy != 0.0f);
}

TEST(motion, slide_axis_is_defined_for_every_anchor) {
    CHECK(slide_axis_for(Anchor::TopLeft) == SlideAxis::FromLeft);
    CHECK(slide_axis_for(Anchor::CenterLeft) == SlideAxis::FromLeft);
    CHECK(slide_axis_for(Anchor::BottomRight) == SlideAxis::FromRight);
    CHECK(slide_axis_for(Anchor::TopCenter) == SlideAxis::FromTop);
    CHECK(slide_axis_for(Anchor::BottomCenter) == SlideAxis::FromBottom);
    CHECK(slide_axis_for(Anchor::Center) == SlideAxis::FromTop);
}
