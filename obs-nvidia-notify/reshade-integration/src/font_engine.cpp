// SPDX-License-Identifier: MIT
#include "font_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>

#include <imgui.h>
// reshade.hpp must follow imgui.h: it supplies the inline definitions for ImDrawList:: that
// route through ReShade's function table.
#include <reshade.hpp>

// stb_truetype, vendored with Dear ImGui. STBTT_STATIC keeps every symbol internal to this
// translation unit, so nothing here can collide with a copy ReShade or the game already has.
// The cost of that is one MSVC warning per entry point the overlay does not call, which says
// nothing about this code and would otherwise fail a warnings-as-errors build.
#if defined(_MSC_VER)
#pragma warning(disable : 4505)   // unreferenced function with internal linkage removed
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

#include "obsn/log.hpp"

namespace obsn::overlay {
namespace {

constexpr char kComponent[] = "fonts";

/// Baked by default: printable ASCII and the Latin-1 supplement. Anything else is added to the
/// atlas the first time it is asked for, so a Cyrillic or accented file name costs one rebuild
/// and then behaves like any other text.
constexpr std::uint32_t kAsciiFirst = 0x20;
constexpr std::uint32_t kAsciiLast = 0x7E;
constexpr std::uint32_t kLatinFirst = 0xA0;
constexpr std::uint32_t kLatinLast = 0xFF;

constexpr int kMinPx = 6;
constexpr int kMaxPx = 96;
constexpr std::size_t kMaxAtlases = 5;   ///< title, list, notifications, chat, and one spare
constexpr int kMaxAtlasDim = 2048;

/// Decodes one UTF-8 sequence. Returns the codepoint and advances `i`. Invalid bytes decode to
/// U+FFFD and consume one byte, so a malformed nickname cannot desynchronise the caller.
std::uint32_t next_codepoint(std::string_view s, std::size_t& i) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = byte(i);
    if (c < 0x80) {
        ++i;
        return c;
    }
    const auto cont = [&](std::size_t k) {
        return k < s.size() && (byte(k) & 0xC0) == 0x80;
    };
    if ((c & 0xE0) == 0xC0 && cont(i + 1)) {
        const std::uint32_t cp = ((c & 0x1Fu) << 6) | (byte(i + 1) & 0x3Fu);
        i += 2;
        return cp < 0x80 ? 0xFFFDu : cp;
    }
    if ((c & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2)) {
        const std::uint32_t cp =
            ((c & 0x0Fu) << 12) | ((byte(i + 1) & 0x3Fu) << 6) | (byte(i + 2) & 0x3Fu);
        i += 3;
        return cp < 0x800 ? 0xFFFDu : cp;
    }
    if ((c & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
        const std::uint32_t cp = ((c & 0x07u) << 18) | ((byte(i + 1) & 0x3Fu) << 12) |
                                 ((byte(i + 2) & 0x3Fu) << 6) | (byte(i + 3) & 0x3Fu);
        i += 4;
        return cp < 0x10000 || cp > 0x10FFFF ? 0xFFFDu : cp;
    }
    ++i;
    return 0xFFFDu;
}

/// Reads a name-table string. TrueType stores these as UTF-16BE for the Microsoft platform and
/// as bytes for the Macintosh one; both appear in fonts in the wild.
std::string name_string(const stbtt_fontinfo& info, int name_id) {
    int length = 0;
    // Microsoft / Unicode BMP / US English first: it is what nearly every shipped font carries.
    const char* raw = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MICROSOFT,
                                              STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH,
                                              name_id);
    if (raw != nullptr && length > 1) {
        std::string out;
        out.reserve(static_cast<std::size_t>(length) / 2);
        for (int i = 0; i + 1 < length; i += 2) {
            const unsigned int hi = static_cast<unsigned char>(raw[i]);
            const unsigned int lo = static_cast<unsigned char>(raw[i + 1]);
            const unsigned int cp = (hi << 8) | lo;
            // Keep it to Latin-1; the label is a menu entry, not a typesetting job.
            if (cp >= 0x20 && cp < 0x100) out.push_back(static_cast<char>(cp));
        }
        if (!out.empty()) return out;
    }
    raw = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN,
                                  STBTT_MAC_LANG_ENGLISH, name_id);
    if (raw != nullptr && length > 0) return std::string(raw, static_cast<std::size_t>(length));
    return {};
}

bool has_font_extension(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc";
}

}  // namespace

std::string FontFile::label() const {
    if (family.empty()) return file;
    if (style.empty() || style == "Regular") return family;
    return family + " " + style;
}

// --- implementation ---------------------------------------------------------------------------

struct FontEngine::Impl {
    struct Glyph {
        float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  ///< offsets from the pen, in pixels
        float advance = 0.0f;
        bool blank = true;   ///< no pixels (space); still advances
    };

    /// One baked size. Glyphs are rasterised at an integer pixel size and scaled by at most a
    /// fraction of a pixel when drawn, which keeps them crisp without an atlas per float.
    struct Atlas {
        int px = 0;
        int width = 0;
        int height = 0;
        float ascent = 0.0f;
        std::unordered_map<std::uint32_t, Glyph> glyphs;
        std::vector<std::uint32_t> pending;   ///< codepoints asked for but not yet baked
        bool dirty = true;
        std::uint64_t texture_handle = 0;
        reshade::api::resource texture{};
        reshade::api::resource_view view{};
        std::uint64_t used_frame = 0;
    };

    std::vector<std::string> dirs;
    std::vector<FontFile> files;
    std::string file;
    int face_index = 0;
    std::vector<unsigned char> font_data;
    stbtt_fontinfo info{};
    bool face_loaded = false;
    int weight = 400;
    std::string error;

    reshade::api::device* device = nullptr;
    std::map<int, Atlas> atlases;
    std::uint64_t frame = 0;

    void unload_textures() {
        if (device != nullptr) {
            for (auto& [px, atlas] : atlases) {
                if (atlas.view.handle != 0) device->destroy_resource_view(atlas.view);
                if (atlas.texture.handle != 0) device->destroy_resource(atlas.texture);
                atlas.view = {};
                atlas.texture = {};
                atlas.texture_handle = 0;
            }
        }
        atlases.clear();
    }

    bool load_face(const std::string& name, int index) {
        font_data.clear();
        face_loaded = false;
        error.clear();
        if (name.empty()) return true;   // back to ReShade's font, not an error

        // Resolve against the folder the scan actually found it in, so a font shipped with
        // the add-on and one the user dropped in their own folder both work.
        std::filesystem::path path;
        for (const FontFile& f : files) {
            if (f.file == name && f.face_index == index) {
                path = std::filesystem::path(f.dir) / name;
                break;
            }
        }
        if (path.empty()) {
            for (const std::string& d : dirs) {
                const std::filesystem::path candidate = std::filesystem::path(d) / name;
                std::error_code exists_ec;
                if (std::filesystem::exists(candidate, exists_ec) && !exists_ec) {
                    path = candidate;
                    break;
                }
            }
        }
        if (path.empty()) {
            error = "'" + name + "' is not in the fonts folder";
            return false;
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size == 0 || size > 64u * 1024u * 1024u) {
            error = "cannot read '" + name + "'";
            return false;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot open '" + name + "'";
            return false;
        }
        font_data.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(font_data.data()), static_cast<std::streamsize>(size));
        if (!in) {
            error = "short read on '" + name + "'";
            font_data.clear();
            return false;
        }
        const int offset = stbtt_GetFontOffsetForIndex(font_data.data(), index);
        if (offset < 0 || stbtt_InitFont(&info, font_data.data(), offset) == 0) {
            error = "'" + name + "' is not a font this build can read";
            font_data.clear();
            return false;
        }
        face_loaded = true;
        return true;
    }

    Atlas* atlas_for(int px) {
        auto it = atlases.find(px);
        if (it != atlases.end()) {
            it->second.used_frame = frame;
            return &it->second;
        }
        if (!face_loaded) return nullptr;
        // Evict the least recently used size rather than growing without bound.
        while (atlases.size() >= kMaxAtlases) {
            auto oldest = atlases.begin();
            for (auto i = atlases.begin(); i != atlases.end(); ++i) {
                if (i->second.used_frame < oldest->second.used_frame) oldest = i;
            }
            if (device != nullptr) {
                if (oldest->second.view.handle != 0) device->destroy_resource_view(oldest->second.view);
                if (oldest->second.texture.handle != 0) device->destroy_resource(oldest->second.texture);
            }
            atlases.erase(oldest);
        }
        Atlas& a = atlases[px];
        a.px = px;
        a.used_frame = frame;
        a.dirty = true;
        for (std::uint32_t cp = kAsciiFirst; cp <= kAsciiLast; ++cp) a.pending.push_back(cp);
        for (std::uint32_t cp = kLatinFirst; cp <= kLatinLast; ++cp) a.pending.push_back(cp);
        return &a;
    }

    /// Rasterises every glyph the atlas knows about into a fresh texture.
    ///
    /// Rebuilding wholesale rather than patching keeps the packer trivial and happens only when
    /// the size or the character set changes, which is to say almost never after the first frame.
    bool rebuild(Atlas& a) {
        if (!face_loaded || device == nullptr) return false;

        std::vector<std::uint32_t> wanted;
        wanted.reserve(a.glyphs.size() + a.pending.size());
        for (const auto& [cp, g] : a.glyphs) wanted.push_back(cp);
        for (std::uint32_t cp : a.pending) {
            if (a.glyphs.find(cp) == a.glyphs.end()) wanted.push_back(cp);
        }
        std::sort(wanted.begin(), wanted.end());
        wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
        a.pending.clear();
        if (wanted.empty()) return false;

        const float scale = stbtt_ScaleForPixelHeight(&info, static_cast<float>(a.px));
        int ascent_i = 0, descent_i = 0, gap_i = 0;
        stbtt_GetFontVMetrics(&info, &ascent_i, &descent_i, &gap_i);
        a.ascent = static_cast<float>(ascent_i) * scale;

        struct Raster {
            std::uint32_t cp = 0;
            int w = 0, h = 0;
            int ox = 0, oy = 0;
            float advance = 0.0f;
            std::vector<unsigned char> bitmap;
        };
        std::vector<Raster> rasters;
        rasters.reserve(wanted.size());

        for (std::uint32_t cp : wanted) {
            Raster r;
            r.cp = cp;
            int advance = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&info, static_cast<int>(cp), &advance, &lsb);
            r.advance = static_cast<float>(advance) * scale;
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetCodepointBitmapBox(&info, static_cast<int>(cp), scale, scale, &x0, &y0, &x1,
                                        &y1);
            r.w = x1 - x0;
            r.h = y1 - y0;
            r.ox = x0;
            r.oy = y0;
            if (r.w > 0 && r.h > 0 && r.w < kMaxAtlasDim && r.h < kMaxAtlasDim) {
                r.bitmap.resize(static_cast<std::size_t>(r.w) * static_cast<std::size_t>(r.h));
                stbtt_MakeCodepointBitmap(&info, r.bitmap.data(), r.w, r.h, r.w, scale, scale,
                                          static_cast<int>(cp));
            } else {
                r.w = 0;
                r.h = 0;
            }
            rasters.push_back(std::move(r));
        }

        // Shelf packing, tallest first. Good enough for a few hundred glyphs and no state to
        // get wrong.
        std::vector<const Raster*> order;
        order.reserve(rasters.size());
        for (const Raster& r : rasters) order.push_back(&r);
        std::stable_sort(order.begin(), order.end(),
                         [](const Raster* l, const Raster* r) { return l->h > r->h; });

        int dim = 128;
        std::unordered_map<std::uint32_t, Glyph> packed;
        bool fits = false;
        while (dim <= kMaxAtlasDim && !fits) {
            packed.clear();
            int pen_x = 1, pen_y = 1, row_h = 0;
            fits = true;
            for (const Raster* r : order) {
                if (r->w == 0 || r->h == 0) continue;
                if (pen_x + r->w + 1 > dim) {
                    pen_x = 1;
                    pen_y += row_h + 1;
                    row_h = 0;
                }
                if (pen_y + r->h + 1 > dim) {
                    fits = false;
                    break;
                }
                Glyph g;
                g.u0 = static_cast<float>(pen_x) / static_cast<float>(dim);
                g.v0 = static_cast<float>(pen_y) / static_cast<float>(dim);
                g.u1 = static_cast<float>(pen_x + r->w) / static_cast<float>(dim);
                g.v1 = static_cast<float>(pen_y + r->h) / static_cast<float>(dim);
                g.x0 = static_cast<float>(r->ox);
                g.y0 = static_cast<float>(r->oy);
                g.x1 = static_cast<float>(r->ox + r->w);
                g.y1 = static_cast<float>(r->oy + r->h);
                g.advance = r->advance;
                g.blank = false;
                packed[r->cp] = g;
                pen_x += r->w + 1;
                row_h = std::max(row_h, r->h);
            }
            if (!fits) dim *= 2;
        }
        if (!fits) {
            error = "font atlas does not fit at this size";
            return false;
        }

        // Blank glyphs (space) still need their advance recorded.
        for (const Raster& r : rasters) {
            if (r.w != 0 && r.h != 0) continue;
            Glyph g;
            g.advance = r.advance;
            g.blank = true;
            packed[r.cp] = g;
        }

        // White with the coverage in alpha, so ImGui's vertex colour tints the glyph.
        std::vector<unsigned char> pixels(static_cast<std::size_t>(dim) *
                                              static_cast<std::size_t>(dim) * 4u,
                                          0u);
        for (const Raster& r : rasters) {
            if (r.w == 0 || r.h == 0) continue;
            const Glyph& g = packed[r.cp];
            const int dst_x = static_cast<int>(std::lround(g.u0 * static_cast<float>(dim)));
            const int dst_y = static_cast<int>(std::lround(g.v0 * static_cast<float>(dim)));
            for (int y = 0; y < r.h; ++y) {
                unsigned char* row =
                    pixels.data() +
                    (static_cast<std::size_t>(dst_y + y) * static_cast<std::size_t>(dim) +
                     static_cast<std::size_t>(dst_x)) * 4u;
                for (int x = 0; x < r.w; ++x) {
                    const unsigned char coverage =
                        r.bitmap[static_cast<std::size_t>(y) * static_cast<std::size_t>(r.w) +
                                 static_cast<std::size_t>(x)];
                    row[x * 4 + 0] = 255;
                    row[x * 4 + 1] = 255;
                    row[x * 4 + 2] = 255;
                    row[x * 4 + 3] = coverage;
                }
            }
        }

        reshade::api::resource texture{};
        reshade::api::resource_view view{};
        const reshade::api::resource_desc desc(
            static_cast<std::uint32_t>(dim), static_cast<std::uint32_t>(dim), 1, 1,
            reshade::api::format::r8g8b8a8_unorm, 1, reshade::api::memory_heap::gpu_only,
            reshade::api::resource_usage::shader_resource);
        reshade::api::subresource_data initial{};
        initial.data = pixels.data();
        initial.row_pitch = static_cast<std::uint32_t>(dim) * 4u;
        initial.slice_pitch = initial.row_pitch * static_cast<std::uint32_t>(dim);
        if (!device->create_resource(desc, &initial,
                                     reshade::api::resource_usage::shader_resource, &texture)) {
            error = "the graphics device refused the font texture";
            return false;
        }
        if (!device->create_resource_view(
                texture, reshade::api::resource_usage::shader_resource,
                reshade::api::resource_view_desc(reshade::api::format::r8g8b8a8_unorm, 0, 1, 0, 1),
                &view)) {
            device->destroy_resource(texture);
            error = "the graphics device refused the font texture view";
            return false;
        }

        if (a.view.handle != 0) device->destroy_resource_view(a.view);
        if (a.texture.handle != 0) device->destroy_resource(a.texture);
        a.texture = texture;
        a.view = view;
        a.texture_handle = view.handle;
        a.width = dim;
        a.height = dim;
        a.glyphs = std::move(packed);
        a.dirty = false;
        error.clear();
        OBSN_INFO(kComponent, "baked " + std::to_string(a.glyphs.size()) + " glyphs at " +
                                  std::to_string(a.px) + "px into a " + std::to_string(dim) + "px atlas");
        return true;
    }

    /// How far the second stroke is offset for the configured weight, in pixels at this size.
    ///
    /// Drawn rather than baked. Dilating the glyph bitmap was the first attempt and it bloated
    /// the text: a box dilation thickens vertically as much as horizontally and rounds every
    /// corner, which at HUD sizes reads as a smear rather than a bold. A designed bold is mostly
    /// a wider stem, so a single extra stroke a fraction of a pixel to the side is both closer
    /// to the real thing and far gentler.
    float bold_offset(float px) const {
        if (weight <= 400) return 0.0f;
        const float t = std::min(1.0f, static_cast<float>(weight - 400) / 300.0f);
        return t * std::max(0.6f, px / 22.0f);
    }

    int key_for(float px) const {
        return std::clamp(static_cast<int>(std::lround(px)), kMinPx, kMaxPx);
    }

    /// The atlas to draw `px` with.
    ///
    /// Asking for a size that has never been used registers it, so the next begin_frame bakes
    /// it; until then this returns nullptr and the caller falls back to ReShade's font. That is
    /// deliberate: baking mid-draw-list would put a stall exactly where a frame can least
    /// afford one.
    Atlas* live_atlas(float px) {
        Atlas* a = atlas_for(key_for(px));
        if (a == nullptr || a->texture_handle == 0) return nullptr;
        return a;
    }
};

FontEngine::FontEngine() : impl_(std::make_unique<Impl>()) {}

FontEngine::~FontEngine() { impl_->unload_textures(); }

void FontEngine::set_directories(std::vector<std::string> dirs) {
    if (impl_->dirs == dirs) return;
    impl_->dirs = std::move(dirs);
    rescan();
}

const std::vector<std::string>& FontEngine::directories() const noexcept { return impl_->dirs; }

void FontEngine::rescan() {
    impl_->files.clear();
    for (const std::string& dir : impl_->dirs) {
    if (dir.empty()) continue;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) continue;
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec) || ec) continue;
        if (!has_font_extension(entry.path())) continue;

        std::error_code size_ec;
        const auto size = std::filesystem::file_size(entry.path(), size_ec);
        if (size_ec || size == 0 || size > 64u * 1024u * 1024u) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) continue;
        std::vector<unsigned char> data(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        if (!in) continue;

        const int faces = std::max(1, stbtt_GetNumberOfFonts(data.data()));
        for (int face = 0; face < faces; ++face) {
            const int offset = stbtt_GetFontOffsetForIndex(data.data(), face);
            stbtt_fontinfo probe{};
            if (offset < 0 || stbtt_InitFont(&probe, data.data(), offset) == 0) continue;
            FontFile f;
            f.file = entry.path().filename().string();
            f.dir = dir;
            f.face_index = face;
            f.family = name_string(probe, 1);
            f.style = name_string(probe, 2);
            // The user's own folder comes first, so a font of the same name there wins.
            const bool already = std::any_of(
                impl_->files.begin(), impl_->files.end(), [&](const FontFile& e) {
                    return e.file == f.file && e.face_index == f.face_index;
                });
            if (!already) impl_->files.push_back(std::move(f));
        }
    }
    }
    std::sort(impl_->files.begin(), impl_->files.end(), [](const FontFile& l, const FontFile& r) {
        return l.label() < r.label();
    });
}

const std::vector<FontFile>& FontEngine::available() const noexcept { return impl_->files; }

bool FontEngine::select(const std::string& file, int face_index) {
    if (impl_->file == file && impl_->face_index == face_index && impl_->error.empty()) {
        return true;
    }
    impl_->unload_textures();
    impl_->file = file;
    impl_->face_index = face_index;
    return impl_->load_face(file, face_index);
}

const std::string& FontEngine::selected_file() const noexcept { return impl_->file; }
int FontEngine::selected_face_index() const noexcept { return impl_->face_index; }

void FontEngine::begin_frame(reshade::api::device* device) {
    if (device != impl_->device) {
        // A different device means every texture we hold belongs to something that is gone.
        impl_->atlases.clear();
        impl_->device = device;
    }
    ++impl_->frame;
    if (!impl_->face_loaded || device == nullptr) return;
    // One rebuild per frame: a font or size change costs a hitch, never a stall.
    for (auto& [px, atlas] : impl_->atlases) {
        if (atlas.dirty || !atlas.pending.empty()) {
            impl_->rebuild(atlas);
            break;
        }
    }
}

void FontEngine::set_weight(int weight) {
    const int clamped = std::clamp(weight, 100, 900);
    if (clamped == impl_->weight) return;
    impl_->weight = clamped;
}

void FontEngine::release() { impl_->unload_textures(); }

bool FontEngine::ready_at(float px) { return impl_->live_atlas(px) != nullptr; }

bool FontEngine::ready() const noexcept {
    if (!impl_->face_loaded || impl_->device == nullptr) return false;
    for (const auto& [px, atlas] : impl_->atlases) {
        if (atlas.texture_handle != 0) return true;
    }
    return false;
}

const std::string& FontEngine::error() const noexcept { return impl_->error; }

std::string FontEngine::status() const {
    if (!impl_->error.empty()) return "ReShade's font (" + impl_->error + ")";
    if (impl_->file.empty()) return "ReShade's font";
    if (!impl_->face_loaded) return "ReShade's font (no face loaded)";
    if (!ready()) return impl_->file + " (baking)";
    return impl_->file;
}

float FontEngine::measure(std::string_view text, float px) {
    if (text.empty()) return 0.0f;
    Impl::Atlas* atlas = impl_->live_atlas(px);
    if (atlas == nullptr) return 0.0f;
    const float scale = px / static_cast<float>(atlas->px);
    const float bold = impl_->bold_offset(px);
    float width = 0.0f;
    std::size_t glyphs = 0;
    for (std::size_t i = 0; i < text.size();) {
        const std::uint32_t cp = next_codepoint(text, i);
        ++glyphs;
        const auto it = atlas->glyphs.find(cp);
        if (it == atlas->glyphs.end()) {
            const auto fallback = atlas->glyphs.find('?');
            atlas->pending.push_back(cp);
            width += fallback == atlas->glyphs.end() ? static_cast<float>(atlas->px) * 0.5f
                                                     : fallback->second.advance;
            continue;
        }
        width += it->second.advance;
    }
    // The extra stroke widens every glyph, so measurement has to know about it too.
    return width * scale + bold * static_cast<float>(glyphs);
}

float FontEngine::ascent(float px) {
    Impl::Atlas* atlas = impl_->live_atlas(px);
    if (atlas == nullptr) return px * 0.8f;
    return atlas->ascent * (px / static_cast<float>(atlas->px));
}

void FontEngine::draw(ImDrawList* dl, float x, float y, float px, std::uint32_t color,
                      std::string_view text) {
    if (dl == nullptr || text.empty()) return;
    Impl::Atlas* atlas = impl_->live_atlas(px);
    if (atlas == nullptr) return;

    const float scale = px / static_cast<float>(atlas->px);
    // Snap the origin to whole pixels. A glyph baked for this exact size and then drawn at a
    // fractional offset is resampled across two columns of pixels, which softens one edge and
    // hardens the other -- the difference between crisp text and text that looks slightly
    // chewed, and the reason an outline drawn from several offsets looked ragged.
    const float origin = std::floor(x + 0.5f);
    const float baseline = std::floor(y + atlas->ascent * scale + 0.5f);

    const float bold = impl_->bold_offset(px);

    dl->PushTextureID(static_cast<ImTextureID>(atlas->texture_handle));
    float pen = origin;
    for (std::size_t i = 0; i < text.size();) {
        const std::uint32_t cp = next_codepoint(text, i);
        auto it = atlas->glyphs.find(cp);
        if (it == atlas->glyphs.end()) {
            atlas->pending.push_back(cp);
            it = atlas->glyphs.find('?');
            if (it == atlas->glyphs.end()) {
                pen += static_cast<float>(atlas->px) * 0.5f * scale;
                continue;
            }
        }
        const Impl::Glyph& g = it->second;
        if (!g.blank) {
            // Whole-pixel quads, so each glyph lands on the texel grid it was baked against.
            const float gx = std::floor(pen + g.x0 * scale + 0.5f);
            const float gy = std::floor(baseline + g.y0 * scale + 0.5f);
            const float gw = (g.x1 - g.x0) * scale;
            const float gh = (g.y1 - g.y0) * scale;
            dl->PrimReserve(6, 4);
            dl->PrimRectUV(ImVec2(gx, gy), ImVec2(gx + gw, gy + gh), ImVec2(g.u0, g.v0),
                           ImVec2(g.u1, g.v1), color);
            if (bold > 0.0f) {
                // One more stroke beside the first: a wider stem, which is what weight is.
                dl->PrimReserve(6, 4);
                dl->PrimRectUV(ImVec2(gx + bold, gy), ImVec2(gx + bold + gw, gy + gh),
                               ImVec2(g.u0, g.v0), ImVec2(g.u1, g.v1), color);
            }
        }
        pen += g.advance * scale + bold;
    }
    dl->PopTextureID();
}

}  // namespace obsn::overlay
