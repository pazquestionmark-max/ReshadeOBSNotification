// SPDX-License-Identifier: MIT
// Vector icon and panel painter.
//
// Every glyph is drawn with ImDrawList primitives rather than a texture. That choice buys:
//   * no texture upload and no ImTextureID to keep alive across a device reset;
//   * clean scaling to any configured size with no mipmaps and no blurring;
//   * no image assets to ship, and so no possibility of shipping anyone else's artwork -- which
//     matters here specifically, because the look this reproduces is a vendor's. The shapes are
//     the ordinary universal marks for record, stop, save and so on; no logo is reproduced.
#ifndef OBSN_ICONS_HPP
#define OBSN_ICONS_HPP

#include <cstdint>

#include "obsn/config.hpp"

struct ImDrawList;

namespace obsn::overlay {

/// Draws `shape` centred on (cx, cy) within a box `size` pixels across.
/// `color` is packed 0xAABBGGRR. A shape of IconShape::None draws nothing.
void draw_icon(ImDrawList* draw_list, IconShape shape, float cx, float cy, float size,
               std::uint32_t color, float thickness = 1.6f);

/// Rounded rectangle with an optional border, used for panels and toasts.
void draw_panel(ImDrawList* draw_list, float x, float y, float w, float h, float rounding,
                std::uint32_t fill, std::uint32_t border, float border_thickness);

/// A vertical two-stop gradient inside a rectangle. Used for the optional panel gradient; the
/// shipped look is flat, so this is only reached when the user asks for it.
void draw_vertical_gradient(ImDrawList* draw_list, float x, float y, float w, float h,
                            std::uint32_t top, std::uint32_t bottom);

/// A soft drop shadow beneath a panel.
///
/// Built from a handful of concentric rounded rectangles of decreasing opacity rather than a
/// blur: it needs no render target, no shader and no second pass, and at the sizes a toast uses
/// the difference is not visible. Costs `steps` rectangles, which is why the count is bounded.
void draw_shadow(ImDrawList* draw_list, float x, float y, float w, float h, float rounding,
                 std::uint32_t color, float size, float offset_y);

}  // namespace obsn::overlay

#endif  // OBSN_ICONS_HPP
