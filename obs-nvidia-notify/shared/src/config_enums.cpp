// SPDX-License-Identifier: MIT
// Enum <-> string tables, colour parsing and the easing curves.
//
// The tables are the single source of truth for the wire and file vocabulary: the settings UI
// builds its combo boxes from them, the serialiser writes through them, and the TypeScript
// schema is checked against them in CI, so the three cannot drift.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "obsn/config.hpp"

namespace obsn {
namespace {

struct Entry {
    const char* name;
    int value;
};

template <typename E, std::size_t N>
const char* name_of(const Entry (&table)[N], E value, const char* fallback) noexcept {
    for (const Entry& e : table) {
        if (e.value == static_cast<int>(value)) return e.name;
    }
    return fallback;
}

template <typename E, std::size_t N>
bool value_of(const Entry (&table)[N], std::string_view text, E& out) noexcept {
    for (const Entry& e : table) {
        if (text == e.name) {
            out = static_cast<E>(e.value);
            return true;
        }
    }
    return false;
}

constexpr Entry kAnchors[] = {
    {"top_left", static_cast<int>(Anchor::TopLeft)},
    {"top_center", static_cast<int>(Anchor::TopCenter)},
    {"top_right", static_cast<int>(Anchor::TopRight)},
    {"center_left", static_cast<int>(Anchor::CenterLeft)},
    {"center", static_cast<int>(Anchor::Center)},
    {"center_right", static_cast<int>(Anchor::CenterRight)},
    {"bottom_left", static_cast<int>(Anchor::BottomLeft)},
    {"bottom_center", static_cast<int>(Anchor::BottomCenter)},
    {"bottom_right", static_cast<int>(Anchor::BottomRight)},
};

constexpr Entry kAligns[] = {
    {"left", static_cast<int>(Align::Left)},
    {"center", static_cast<int>(Align::Center)},
    {"right", static_cast<int>(Align::Right)},
};

constexpr Entry kIcons[] = {
    {"none", static_cast<int>(IconShape::None)},
    {"dot", static_cast<int>(IconShape::Dot)},
    {"circle", static_cast<int>(IconShape::Circle)},
    {"ring", static_cast<int>(IconShape::Ring)},
    {"square", static_cast<int>(IconShape::Square)},
    {"diamond", static_cast<int>(IconShape::Diamond)},
    {"triangle", static_cast<int>(IconShape::Triangle)},
    {"star", static_cast<int>(IconShape::Star)},
    {"chevron", static_cast<int>(IconShape::Chevron)},
    {"record", static_cast<int>(IconShape::Record)},
    {"record_ring", static_cast<int>(IconShape::RecordRing)},
    {"stop", static_cast<int>(IconShape::Stop)},
    {"pause", static_cast<int>(IconShape::Pause)},
    {"play", static_cast<int>(IconShape::Play)},
    {"save", static_cast<int>(IconShape::Save)},
    {"replay", static_cast<int>(IconShape::Replay)},
    {"clock", static_cast<int>(IconShape::Clock)},
    {"camera", static_cast<int>(IconShape::Camera)},
    {"broadcast", static_cast<int>(IconShape::Broadcast)},
    {"film", static_cast<int>(IconShape::Film)},
    {"scissors", static_cast<int>(IconShape::Scissors)},
    {"warning", static_cast<int>(IconShape::Warning)},
    {"check", static_cast<int>(IconShape::Check)},
    {"cross", static_cast<int>(IconShape::Cross)},
    {"folder", static_cast<int>(IconShape::Folder)},
    {"layers", static_cast<int>(IconShape::Layers)},
    {"disk", static_cast<int>(IconShape::Disk)},
};

constexpr Entry kEasings[] = {
    {"linear", static_cast<int>(Easing::Linear)},
    {"ease_in", static_cast<int>(Easing::EaseIn)},
    {"ease_out", static_cast<int>(Easing::EaseOut)},
    {"ease_in_out", static_cast<int>(Easing::EaseInOut)},
    {"ease_in_cubic", static_cast<int>(Easing::EaseInCubic)},
    {"ease_out_cubic", static_cast<int>(Easing::EaseOutCubic)},
    {"ease_in_out_cubic", static_cast<int>(Easing::EaseInOutCubic)},
    {"ease_out_quint", static_cast<int>(Easing::EaseOutQuint)},
    {"ease_out_expo", static_cast<int>(Easing::EaseOutExpo)},
    {"ease_out_back", static_cast<int>(Easing::EaseOutBack)},
    {"ease_out_elastic", static_cast<int>(Easing::EaseOutElastic)},
};

constexpr Entry kOverflows[] = {
    {"clip", static_cast<int>(OverflowMode::Clip)},
    {"ellipsis", static_cast<int>(OverflowMode::Ellipsis)},
    {"wrap", static_cast<int>(OverflowMode::Wrap)},
    {"shrink", static_cast<int>(OverflowMode::Shrink)},
    {"scroll", static_cast<int>(OverflowMode::Scroll)},
};

constexpr Entry kStacks[] = {
    {"down", static_cast<int>(StackDirection::Down)},
    {"up", static_cast<int>(StackDirection::Up)},
};

constexpr Entry kBorders[] = {
    {"none", static_cast<int>(NotificationBorder::None)},
    {"accent", static_cast<int>(NotificationBorder::Accent)},
    {"custom", static_cast<int>(NotificationBorder::Custom)},
};

constexpr Entry kAccentStyles[] = {
    {"none", static_cast<int>(AccentStyle::None)},
    {"bar", static_cast<int>(AccentStyle::Bar)},
    {"tile", static_cast<int>(AccentStyle::Tile)},
    {"bar_and_tile", static_cast<int>(AccentStyle::BarAndTile)},
};

constexpr Entry kMotions[] = {
    {"none", static_cast<int>(MotionKind::None)},
    {"slide_from_edge", static_cast<int>(MotionKind::SlideFromEdge)},
    {"slide_horizontal", static_cast<int>(MotionKind::SlideHorizontal)},
    {"slide_vertical", static_cast<int>(MotionKind::SlideVertical)},
    {"scale", static_cast<int>(MotionKind::Scale)},
};

int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

const char* to_string(Anchor v) noexcept { return name_of(kAnchors, v, "top_right"); }
const char* to_string(Align v) noexcept { return name_of(kAligns, v, "left"); }
const char* to_string(IconShape v) noexcept { return name_of(kIcons, v, "none"); }
const char* to_string(Easing v) noexcept { return name_of(kEasings, v, "linear"); }
const char* to_string(OverflowMode v) noexcept { return name_of(kOverflows, v, "ellipsis"); }
const char* to_string(StackDirection v) noexcept { return name_of(kStacks, v, "down"); }
const char* to_string(NotificationBorder v) noexcept { return name_of(kBorders, v, "none"); }
const char* to_string(AccentStyle v) noexcept { return name_of(kAccentStyles, v, "tile"); }
const char* to_string(MotionKind v) noexcept { return name_of(kMotions, v, "slide_from_edge"); }

bool parse_enum(std::string_view t, Anchor& o) noexcept { return value_of(kAnchors, t, o); }
bool parse_enum(std::string_view t, Align& o) noexcept { return value_of(kAligns, t, o); }
bool parse_enum(std::string_view t, IconShape& o) noexcept { return value_of(kIcons, t, o); }
bool parse_enum(std::string_view t, Easing& o) noexcept { return value_of(kEasings, t, o); }
bool parse_enum(std::string_view t, OverflowMode& o) noexcept { return value_of(kOverflows, t, o); }
bool parse_enum(std::string_view t, StackDirection& o) noexcept { return value_of(kStacks, t, o); }
bool parse_enum(std::string_view t, NotificationBorder& o) noexcept {
    return value_of(kBorders, t, o);
}
bool parse_enum(std::string_view t, AccentStyle& o) noexcept {
    return value_of(kAccentStyles, t, o);
}
bool parse_enum(std::string_view t, MotionKind& o) noexcept { return value_of(kMotions, t, o); }

float ease(Easing easing, float t) noexcept {
    // Clamped at the boundary rather than extrapolated: a caller that has run past the end of an
    // animation wants the end state, not a curve continued past where it means anything.
    if (!(t > 0.0f)) return 0.0f;   // also catches NaN
    if (t >= 1.0f) return 1.0f;

    switch (easing) {
        case Easing::Linear:
            return t;
        case Easing::EaseIn:
            return t * t;
        case Easing::EaseOut:
            return t * (2.0f - t);
        case Easing::EaseInOut:
            return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
        case Easing::EaseInCubic:
            return t * t * t;
        case Easing::EaseOutCubic: {
            const float u = 1.0f - t;
            return 1.0f - u * u * u;
        }
        case Easing::EaseInOutCubic:
            return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
        case Easing::EaseOutQuint: {
            const float u = 1.0f - t;
            return 1.0f - u * u * u * u * u;
        }
        case Easing::EaseOutExpo:
            return 1.0f - std::pow(2.0f, -10.0f * t);
        case Easing::EaseOutBack: {
            // The classic overshoot constants. A toast that settles back into place reads as
            // deliberate; the same curve on the way out reads as a flicker, which is why the
            // exit easing is a separate setting.
            constexpr float c1 = 1.70158f;
            constexpr float c3 = c1 + 1.0f;
            const float u = t - 1.0f;
            return 1.0f + c3 * u * u * u + c1 * u * u;
        }
        case Easing::EaseOutElastic: {
            constexpr float c4 = 2.0943951f;  // (2 * pi) / 3
            return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
        }
    }
    return t;
}

std::optional<Color> Color::from_hex(std::string_view text) {
    std::string_view s = text;
    if (!s.empty() && s.front() == '#') s.remove_prefix(1);
    if (s.size() != 3 && s.size() != 4 && s.size() != 6 && s.size() != 8) return std::nullopt;

    int digits[8] = {};
    for (std::size_t i = 0; i < s.size(); ++i) {
        const int d = hex_digit(s[i]);
        if (d < 0) return std::nullopt;
        digits[i] = d;
    }

    Color c;
    if (s.size() == 3 || s.size() == 4) {
        // Short form doubles each nibble, so #f00 is #ff0000 rather than #f00000.
        c.r = static_cast<std::uint8_t>(digits[0] * 17);
        c.g = static_cast<std::uint8_t>(digits[1] * 17);
        c.b = static_cast<std::uint8_t>(digits[2] * 17);
        c.a = s.size() == 4 ? static_cast<std::uint8_t>(digits[3] * 17) : std::uint8_t{255};
    } else {
        c.r = static_cast<std::uint8_t>(digits[0] * 16 + digits[1]);
        c.g = static_cast<std::uint8_t>(digits[2] * 16 + digits[3]);
        c.b = static_cast<std::uint8_t>(digits[4] * 16 + digits[5]);
        c.a = s.size() == 8 ? static_cast<std::uint8_t>(digits[6] * 16 + digits[7])
                            : std::uint8_t{255};
    }
    return c;
}

std::string Color::to_hex() const {
    char buffer[10];
    const int n = std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X",
                                static_cast<unsigned>(r), static_cast<unsigned>(g),
                                static_cast<unsigned>(b), static_cast<unsigned>(a));
    return std::string(buffer, static_cast<std::size_t>(n > 0 ? n : 0));
}

Color Color::with_alpha_scale(float s) const noexcept {
    const float scaled = static_cast<float>(a) * std::clamp(s, 0.0f, 1.0f);
    Color out = *this;
    out.a = static_cast<std::uint8_t>(std::clamp(scaled + 0.5f, 0.0f, 255.0f));
    return out;
}

Color Color::mix(const Color& other, float t) const noexcept {
    const float u = std::clamp(t, 0.0f, 1.0f);
    const auto lerp = [u](std::uint8_t from, std::uint8_t to) {
        const float value = static_cast<float>(from) + (static_cast<float>(to) -
                                                        static_cast<float>(from)) * u;
        return static_cast<std::uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
    };
    return Color{lerp(r, other.r), lerp(g, other.g), lerp(b, other.b), lerp(a, other.a)};
}

}  // namespace obsn
