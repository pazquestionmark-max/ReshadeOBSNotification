// SPDX-License-Identifier: MIT
// A typeface of the overlay's own, independent of ReShade's.
//
// ReShade owns the Dear ImGui font atlas, and that atlas is deliberately absent from the
// function table ReShade exports to add-ons -- reading it is how the font picker crashed the
// game. So this does not touch it. It rasterises a .ttf itself with stb_truetype, uploads the
// result as an ordinary texture through ReShade's device API, and draws glyph quads from it.
//
// Two consequences worth stating plainly:
//   * The font applies to the overlay alone. ReShade's own UI keeps its own font, which is what
//     the user asked for.
//   * Text measurement becomes ours. Widths come from the same advance table the glyphs are
//     drawn from, so right-alignment can no longer disagree with what is on screen.
//
// Every failure path falls back to ReShade's font rather than drawing nothing.
#ifndef OBSN_FONT_ENGINE_HPP
#define OBSN_FONT_ENGINE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ImDrawList;

namespace reshade::api {
struct device;
}

namespace obsn::overlay {

/// A font file found in the overlay's `fonts` folder.
struct FontFile {
    std::string file;      ///< file name, which is what the configuration stores
    std::string dir;       ///< the folder it was found in
    std::string family;    ///< name table entry 1, e.g. "Roboto"
    std::string style;     ///< name table entry 2, e.g. "Medium"
    int face_index = 0;    ///< index within a .ttc collection; 0 for .ttf/.otf

    /// "Roboto Medium", or the file name when the name table could not be read.
    std::string label() const;
};

class FontEngine {
public:
    FontEngine();
    ~FontEngine();
    FontEngine(const FontEngine&) = delete;
    FontEngine& operator=(const FontEngine&) = delete;

    /// Where to look for .ttf/.otf/.ttc files: the user's own folder first, then the one
    /// shipped beside the add-on. Rescans on change.
    void set_directories(std::vector<std::string> dirs);
    const std::vector<std::string>& directories() const noexcept;
    void rescan();
    const std::vector<FontFile>& available() const noexcept;

    /// Loads a face. An empty file name returns the overlay to ReShade's font.
    /// Returns false and sets error() when the file cannot be used.
    bool select(const std::string& file, int face_index);
    /// Stroke weight on the 100-900 scale. Anything above 400 is emboldened when glyphs are
    /// rasterised, which is what gives a 700 from a face that ships a single weight. Changing it
    /// rebakes, so it is cheap to call every frame with the same value.
    void set_weight(int weight);
    const std::string& selected_file() const noexcept;
    int selected_face_index() const noexcept;

    /// Called once per frame before any drawing, with the device the overlay is rendering on.
    /// Uploads at most one atlas per frame so a font change costs a hitch, not a stall.
    void begin_frame(reshade::api::device* device);
    /// Releases every texture. Safe to call with a device that is already gone.
    void release();

    /// True when a face is loaded and its texture is live, i.e. measure/draw are usable.
    bool ready() const noexcept;
    const std::string& error() const noexcept;
    /// Human-readable, for the diagnostics panel.
    std::string status() const;

    /// Width of `text` at `px`, or 0 when the engine cannot serve this frame -- the caller
    /// falls back to ReShade's font, which is also what happens for the frame or two after a
    /// new size is first asked for and its atlas is still baking.
    float measure(std::string_view text, float px);
    /// Distance from the top of the line box to the baseline, at `px`.
    float ascent(float px);
    /// Draws `text` with its top-left at (x, y). Silently does nothing when not ready, so the
    /// caller must check ready_at() first if it needs to choose a path.
    void draw(ImDrawList* dl, float x, float y, float px, std::uint32_t color,
              std::string_view text);
    /// True when this exact size can be drawn right now.
    bool ready_at(float px);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace obsn::overlay

#endif  // OBSN_FONT_ENGINE_HPP
