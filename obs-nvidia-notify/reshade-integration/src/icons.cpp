// SPDX-License-Identifier: MIT
#include "icons.hpp"

#include <algorithm>
#include <cmath>

#include <imgui.h>
// reshade.hpp must follow imgui.h: it supplies the inline definitions for ImDrawList:: that
// route through ReShade's function table. imgui.h alone only declares them, so omitting this
// compiles cleanly and then fails at link with unresolved externals.
#include <reshade.hpp>

namespace obsn::overlay {
namespace {

constexpr float kPi = 3.14159265358979323846f;

ImVec2 at(float cx, float cy, float x, float y) { return ImVec2(cx + x, cy + y); }

/// Splits the packed colour so an alpha can be scaled without unpacking at every call site.
std::uint32_t scale_alpha(std::uint32_t color, float scale) {
    const float a = static_cast<float>((color >> 24) & 0xFFu) * std::clamp(scale, 0.0f, 1.0f);
    return (color & 0x00FFFFFFu) |
           (static_cast<std::uint32_t>(std::clamp(a + 0.5f, 0.0f, 255.0f)) << 24);
}

/// A filled triangle pointing right, used for Play and as the arrowhead on Replay.
void filled_triangle_right(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color) {
    dl->AddTriangleFilled(at(cx, cy, -r * 0.55f, -r), at(cx, cy, -r * 0.55f, r),
                          at(cx, cy, r * 0.85f, 0.0f), color);
}

/// The two-bar pause mark, also the basis of the recording indicator's paused state.
void pause_bars(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color) {
    const float w = r * 0.34f;
    const float gap = r * 0.30f;
    dl->AddRectFilled(at(cx, cy, -gap - w, -r), at(cx, cy, -gap, r), color);
    dl->AddRectFilled(at(cx, cy, gap, -r), at(cx, cy, gap + w, r), color);
}

/// A downward arrow into an open tray: the universal "written to disk" mark.
void save_glyph(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color, float t) {
    // Shaft and head, sized so the arrow reads at 16px as well as at 40.
    const float head = r * 0.46f;
    dl->AddLine(at(cx, cy, 0.0f, -r * 0.95f), at(cx, cy, 0.0f, r * 0.15f), color, t);
    dl->AddTriangleFilled(at(cx, cy, -head, r * 0.02f), at(cx, cy, head, r * 0.02f),
                          at(cx, cy, 0.0f, r * 0.62f), color);
    // The tray, open at the top so the arrow is seen to enter it.
    dl->AddLine(at(cx, cy, -r * 0.82f, r * 0.52f), at(cx, cy, -r * 0.82f, r * 0.92f), color, t);
    dl->AddLine(at(cx, cy, -r * 0.82f, r * 0.92f), at(cx, cy, r * 0.82f, r * 0.92f), color, t);
    dl->AddLine(at(cx, cy, r * 0.82f, r * 0.92f), at(cx, cy, r * 0.82f, r * 0.52f), color, t);
}

/// An anticlockwise circular arrow: rewind, and so "replay".
void replay_glyph(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color, float t) {
    // An arc with a deliberate gap, so the arrowhead has somewhere to sit and the shape does
    // not read as a plain ring.
    dl->PathArcTo(ImVec2(cx, cy), r * 0.78f, kPi * 0.62f, kPi * 2.35f, 24);
    dl->PathStroke(color, ImDrawFlags_None, t);

    // The head, at the arc's start, pointing back the way the arc came.
    const float angle = kPi * 0.62f;
    const float hx = cx + std::cos(angle) * r * 0.78f;
    const float hy = cy + std::sin(angle) * r * 0.78f;
    const float head = r * 0.40f;
    dl->AddTriangleFilled(ImVec2(hx - head * 0.2f, hy + head),
                          ImVec2(hx - head, hy - head * 0.35f),
                          ImVec2(hx + head * 0.55f, hy - head * 0.5f), color);
}

/// A dot with two arcs radiating from it: broadcasting.
void broadcast_glyph(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color, float t) {
    dl->AddCircleFilled(ImVec2(cx, cy), r * 0.26f, color, 16);
    for (int ring = 1; ring <= 2; ++ring) {
        const float radius = r * (0.30f + 0.32f * static_cast<float>(ring));
        // Two opposing wedges rather than full circles: the gaps are what make it read as
        // signal rather than as a target.
        dl->PathArcTo(ImVec2(cx, cy), radius, -kPi * 0.36f, kPi * 0.36f, 16);
        dl->PathStroke(color, ImDrawFlags_None, t);
        dl->PathArcTo(ImVec2(cx, cy), radius, kPi * 0.64f, kPi * 1.36f, 16);
        dl->PathStroke(color, ImDrawFlags_None, t);
    }
}

void camera_glyph(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color, float t) {
    const float body_w = r * 0.86f;
    const float body_h = r * 0.62f;
    dl->AddRect(at(cx, cy, -r, -body_h), at(cx, cy, body_w, body_h), color, r * 0.16f,
                ImDrawFlags_None, t);
    // The lens barrel, as a wedge off the right-hand side.
    dl->AddTriangleFilled(at(cx, cy, body_w + r * 0.08f, -body_h * 0.75f),
                          at(cx, cy, body_w + r * 0.08f, body_h * 0.75f),
                          at(cx, cy, r, 0.0f), color);
    dl->AddCircleFilled(at(cx, cy, -r * 0.30f, 0.0f), r * 0.24f, color, 14);
}

void film_glyph(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color, float t) {
    dl->AddRect(at(cx, cy, -r, -r * 0.74f), at(cx, cy, r, r * 0.74f), color, r * 0.12f,
                ImDrawFlags_None, t);
    // Sprocket holes down both edges.
    for (int i = -1; i <= 1; ++i) {
        const float y = static_cast<float>(i) * r * 0.44f;
        dl->AddRectFilled(at(cx, cy, -r * 0.88f, y - r * 0.12f),
                          at(cx, cy, -r * 0.58f, y + r * 0.12f), color);
        dl->AddRectFilled(at(cx, cy, r * 0.58f, y - r * 0.12f),
                          at(cx, cy, r * 0.88f, y + r * 0.12f), color);
    }
}

void warning_glyph(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color, float t) {
    // The triangle is stroked rather than filled, so the bar and dot inside it stay legible
    // when the icon is drawn small.
    dl->AddTriangle(at(cx, cy, 0.0f, -r), at(cx, cy, r, r * 0.8f), at(cx, cy, -r, r * 0.8f),
                    color, t);
    dl->AddLine(at(cx, cy, 0.0f, -r * 0.34f), at(cx, cy, 0.0f, r * 0.22f), color, t * 1.15f);
    dl->AddCircleFilled(at(cx, cy, 0.0f, r * 0.50f), std::max(1.0f, t * 0.62f), color, 8);
}

void folder_glyph(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color, float t) {
    // The tab along the top edge is what distinguishes a folder from a plain rectangle.
    dl->AddLine(at(cx, cy, -r, -r * 0.62f), at(cx, cy, -r * 0.18f, -r * 0.62f), color, t);
    dl->AddLine(at(cx, cy, -r * 0.18f, -r * 0.62f), at(cx, cy, 0.05f, -r * 0.28f), color, t);
    dl->AddRect(at(cx, cy, -r, -r * 0.28f), at(cx, cy, r, r * 0.70f), color, r * 0.12f,
                ImDrawFlags_None, t);
    dl->AddLine(at(cx, cy, -r, -r * 0.62f), at(cx, cy, -r, -r * 0.28f), color, t);
}

void layers_glyph(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color, float t) {
    // Three stacked sheets seen edge-on, which is what a scene list looks like.
    for (int i = 0; i < 3; ++i) {
        const float y = (static_cast<float>(i) - 1.0f) * r * 0.46f;
        const float w = r * (1.0f - static_cast<float>(i) * 0.07f);
        dl->AddLine(at(cx, cy, -w, y), at(cx, cy, w, y), color, t * 1.25f);
    }
}

void clock_glyph(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color, float t) {
    dl->AddCircle(ImVec2(cx, cy), r * 0.9f, color, 24, t);
    dl->AddLine(ImVec2(cx, cy), at(cx, cy, 0.0f, -r * 0.55f), color, t);
    dl->AddLine(ImVec2(cx, cy), at(cx, cy, r * 0.40f, 0.0f), color, t);
}

void disk_glyph(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color, float t) {
    dl->AddCircle(ImVec2(cx, cy), r * 0.92f, color, 28, t);
    dl->AddCircleFilled(ImVec2(cx, cy), r * 0.22f, color, 14);
}

void scissors_glyph(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color, float t) {
    dl->AddLine(at(cx, cy, -r * 0.7f, -r * 0.9f), at(cx, cy, r * 0.55f, r * 0.45f), color, t);
    dl->AddLine(at(cx, cy, r * 0.7f, -r * 0.9f), at(cx, cy, -r * 0.55f, r * 0.45f), color, t);
    dl->AddCircle(at(cx, cy, -r * 0.5f, r * 0.62f), r * 0.28f, color, 14, t);
    dl->AddCircle(at(cx, cy, r * 0.5f, r * 0.62f), r * 0.28f, color, 14, t);
}

void star_glyph(ImDrawList* dl, float cx, float cy, float r, std::uint32_t color) {
    ImVec2 points[10];
    for (int i = 0; i < 10; ++i) {
        // Alternating outer and inner radii, starting at the top.
        const float radius = (i % 2 == 0) ? r : r * 0.42f;
        const float angle = -kPi * 0.5f + static_cast<float>(i) * kPi / 5.0f;
        points[i] = ImVec2(cx + std::cos(angle) * radius, cy + std::sin(angle) * radius);
    }
    dl->AddConvexPolyFilled(points, 10, color);
}

}  // namespace

void draw_icon(ImDrawList* dl, IconShape shape, float cx, float cy, float size,
               std::uint32_t color, float thickness) {
    if (dl == nullptr || shape == IconShape::None || size <= 0.0f) return;
    const float r = size * 0.5f;
    const float t = std::max(1.0f, thickness);

    switch (shape) {
        case IconShape::None:
            return;
        case IconShape::Dot:
            dl->AddCircleFilled(ImVec2(cx, cy), r * 0.5f, color, 12);
            return;
        case IconShape::Circle:
        case IconShape::Record:
            dl->AddCircleFilled(ImVec2(cx, cy), r * 0.82f, color, 24);
            return;
        case IconShape::Ring:
            dl->AddCircle(ImVec2(cx, cy), r * 0.82f, color, 24, t);
            return;
        case IconShape::RecordRing:
            // Armed but not writing: the same dot, held inside a ring.
            dl->AddCircle(ImVec2(cx, cy), r * 0.92f, color, 24, t);
            dl->AddCircleFilled(ImVec2(cx, cy), r * 0.42f, color, 16);
            return;
        case IconShape::Square:
            dl->AddRectFilled(at(cx, cy, -r * 0.72f, -r * 0.72f), at(cx, cy, r * 0.72f, r * 0.72f),
                              color, r * 0.12f);
            return;
        case IconShape::Stop:
            dl->AddRectFilled(at(cx, cy, -r * 0.66f, -r * 0.66f), at(cx, cy, r * 0.66f, r * 0.66f),
                              color, r * 0.10f);
            return;
        case IconShape::Pause:
            pause_bars(dl, cx, cy, r * 0.72f, color);
            return;
        case IconShape::Play:
            filled_triangle_right(dl, cx, cy, r * 0.8f, color);
            return;
        case IconShape::Diamond: {
            const ImVec2 points[4] = {at(cx, cy, 0.0f, -r), at(cx, cy, r, 0.0f),
                                      at(cx, cy, 0.0f, r), at(cx, cy, -r, 0.0f)};
            dl->AddConvexPolyFilled(points, 4, color);
            return;
        }
        case IconShape::Triangle:
            dl->AddTriangleFilled(at(cx, cy, 0.0f, -r), at(cx, cy, r * 0.9f, r * 0.75f),
                                  at(cx, cy, -r * 0.9f, r * 0.75f), color);
            return;
        case IconShape::Star:
            star_glyph(dl, cx, cy, r, color);
            return;
        case IconShape::Chevron:
            dl->AddLine(at(cx, cy, -r * 0.45f, -r * 0.7f), at(cx, cy, r * 0.4f, 0.0f), color, t);
            dl->AddLine(at(cx, cy, r * 0.4f, 0.0f), at(cx, cy, -r * 0.45f, r * 0.7f), color, t);
            return;
        case IconShape::Save:
            save_glyph(dl, cx, cy, r * 0.88f, color, t);
            return;
        case IconShape::Replay:
            replay_glyph(dl, cx, cy, r, color, t);
            return;
        case IconShape::Clock:
            clock_glyph(dl, cx, cy, r, color, t);
            return;
        case IconShape::Camera:
            camera_glyph(dl, cx, cy, r * 0.92f, color, t);
            return;
        case IconShape::Broadcast:
            broadcast_glyph(dl, cx, cy, r, color, t);
            return;
        case IconShape::Film:
            film_glyph(dl, cx, cy, r * 0.94f, color, t);
            return;
        case IconShape::Scissors:
            scissors_glyph(dl, cx, cy, r * 0.9f, color, t);
            return;
        case IconShape::Warning:
            warning_glyph(dl, cx, cy, r * 0.9f, color, t);
            return;
        case IconShape::Check:
            dl->AddLine(at(cx, cy, -r * 0.7f, 0.0f), at(cx, cy, -r * 0.18f, r * 0.55f), color,
                        t * 1.2f);
            dl->AddLine(at(cx, cy, -r * 0.18f, r * 0.55f), at(cx, cy, r * 0.72f, -r * 0.6f),
                        color, t * 1.2f);
            return;
        case IconShape::Cross:
            dl->AddLine(at(cx, cy, -r * 0.6f, -r * 0.6f), at(cx, cy, r * 0.6f, r * 0.6f), color,
                        t * 1.2f);
            dl->AddLine(at(cx, cy, r * 0.6f, -r * 0.6f), at(cx, cy, -r * 0.6f, r * 0.6f), color,
                        t * 1.2f);
            return;
        case IconShape::Folder:
            folder_glyph(dl, cx, cy, r * 0.9f, color, t);
            return;
        case IconShape::Layers:
            layers_glyph(dl, cx, cy, r * 0.88f, color, t);
            return;
        case IconShape::Disk:
            disk_glyph(dl, cx, cy, r, color, t);
            return;
    }
}

void draw_panel(ImDrawList* dl, float x, float y, float w, float h, float rounding,
                std::uint32_t fill, std::uint32_t border, float border_thickness) {
    if (dl == nullptr || w <= 0.0f || h <= 0.0f) return;
    // A rounding larger than half the shorter side produces a degenerate shape in ImGui's
    // path builder, so it is capped here rather than at every call site.
    const float r = std::clamp(rounding, 0.0f, std::min(w, h) * 0.5f);
    const ImVec2 min(x, y);
    const ImVec2 max(x + w, y + h);
    if ((fill >> 24) != 0u) dl->AddRectFilled(min, max, fill, r);
    if ((border >> 24) != 0u && border_thickness > 0.0f) {
        dl->AddRect(min, max, border, r, ImDrawFlags_None, border_thickness);
    }
}

void draw_vertical_gradient(ImDrawList* dl, float x, float y, float w, float h,
                            std::uint32_t top, std::uint32_t bottom) {
    if (dl == nullptr || w <= 0.0f || h <= 0.0f) return;
    dl->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + h), top, top, bottom, bottom);
}

void draw_shadow(ImDrawList* dl, float x, float y, float w, float h, float rounding,
                 std::uint32_t color, float size, float offset_y) {
    if (dl == nullptr || size <= 0.0f || (color >> 24) == 0u) return;

    // Six layers is where the banding stops being visible at the sizes a toast uses; more is
    // paid for in triangles every frame and buys nothing anyone can see.
    constexpr int kSteps = 6;
    for (int i = kSteps; i >= 1; --i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSteps);
        const float spread = size * t;
        // Opacity falls off with the square of the distance, which is what a real penumbra does
        // and what keeps the innermost layer from reading as a hard second edge.
        const float alpha = (1.0f - t) * (1.0f - t) * 0.9f;
        draw_panel(dl, x - spread, y - spread + offset_y, w + spread * 2.0f, h + spread * 2.0f,
                   rounding + spread, scale_alpha(color, alpha), 0u, 0.0f);
    }
}

}  // namespace obsn::overlay
