// SPDX-License-Identifier: MIT
// Configuration serialisation, parsing, repair and migration.
//
// The reading helpers all share one shape: take the value if it is present and of the right
// type, otherwise keep the default and record why. That is what makes "loading never fails"
// true by construction rather than by every call site remembering to check.
#include "obsn/config.hpp"

#include <algorithm>
#include <cmath>

namespace obsn {
namespace {

using Sev = ConfigIssue::Severity;

std::string join(std::string_view a, std::string_view b) {
    std::string out;
    out.reserve(a.size() + b.size() + 1);
    out.append(a);
    if (!a.empty()) out.push_back('.');
    out.append(b);
    return out;
}

void read_bool(const json::Value& v, std::string_view key, bool& out) {
    if (const json::Value* found = v.find(key); found != nullptr && found->is_bool()) {
        out = found->as_bool();
    }
}

void read_string(const json::Value& v, std::string_view key, std::string& out) {
    if (const json::Value* found = v.find(key); found != nullptr && found->is_string()) {
        out = found->as_string();
    }
}

/// Numbers are clamped rather than rejected, and every clamp is reported. A configuration that
/// asks for a 900-pixel font is a mistake to point out, not a reason to refuse to start.
void read_float(const json::Value& v, std::string_view key, float& out, float lo, float hi,
                std::string_view path, ConfigDiagnostics& diag) {
    const json::Value* found = v.find(key);
    if (found == nullptr) return;
    if (!found->is_number()) {
        diag.add(Sev::Warning, join(path, key), "expected a number; kept the default");
        return;
    }
    const double raw = found->as_double();
    if (!std::isfinite(raw)) {
        diag.add(Sev::Warning, join(path, key), "not a finite number; kept the default");
        return;
    }
    const double clamped = std::clamp(raw, static_cast<double>(lo), static_cast<double>(hi));
    if (clamped != raw) {
        diag.add(Sev::Info, join(path, key),
                 "out of range; clamped to " + std::to_string(clamped));
    }
    out = static_cast<float>(clamped);
}

void read_int(const json::Value& v, std::string_view key, int& out, int lo, int hi,
              std::string_view path, ConfigDiagnostics& diag) {
    const json::Value* found = v.find(key);
    if (found == nullptr) return;
    if (!found->is_number()) {
        diag.add(Sev::Warning, join(path, key), "expected a number; kept the default");
        return;
    }
    const long long raw = found->as_int();
    const long long clamped = std::clamp<long long>(raw, lo, hi);
    if (clamped != raw) {
        diag.add(Sev::Info, join(path, key),
                 "out of range; clamped to " + std::to_string(clamped));
    }
    out = static_cast<int>(clamped);
}

void read_color(const json::Value& v, std::string_view key, Color& out, std::string_view path,
                ConfigDiagnostics& diag) {
    const json::Value* found = v.find(key);
    if (found == nullptr) return;
    if (!found->is_string()) {
        diag.add(Sev::Warning, join(path, key), "expected a colour string; kept the default");
        return;
    }
    if (const std::optional<Color> parsed = Color::from_hex(found->as_string())) {
        out = *parsed;
    } else {
        diag.add(Sev::Warning, join(path, key),
                 "not a valid #RGB/#RGBA/#RRGGBB/#RRGGBBAA colour; kept the default");
    }
}

template <typename E>
void read_enum(const json::Value& v, std::string_view key, E& out, std::string_view path,
               ConfigDiagnostics& diag) {
    const json::Value* found = v.find(key);
    if (found == nullptr) return;
    if (!found->is_string()) {
        diag.add(Sev::Warning, join(path, key), "expected a string; kept the default");
        return;
    }
    E parsed = out;
    if (parse_enum(found->as_string(), parsed)) {
        out = parsed;
    } else {
        diag.add(Sev::Warning, join(path, key),
                 "'" + found->as_string() + "' is not a recognised value; kept the default");
    }
}

// --- composite pieces ------------------------------------------------------------------------

json::Value write(const Placement& p) {
    json::Object o;
    o.emplace_back("visible", json::Value(p.visible));
    o.emplace_back("anchor", json::Value(to_string(p.anchor)));
    o.emplace_back("x", json::Value(p.x));
    o.emplace_back("y", json::Value(p.y));
    o.emplace_back("percent", json::Value(p.percent));
    o.emplace_back("align", json::Value(to_string(p.align)));
    return json::Value(std::move(o));
}

void read(const json::Value& v, std::string_view key, Placement& p, std::string_view path,
          ConfigDiagnostics& diag) {
    const json::Value* found = v.find(key);
    if (found == nullptr || !found->is_object()) return;
    const std::string here = join(path, key);
    read_bool(*found, "visible", p.visible);
    read_enum(*found, "anchor", p.anchor, here, diag);
    // Percentage offsets are fractions, pixel offsets are pixels, so one range cannot serve
    // both. The wider bound is applied here and the semantic check happens in clamp().
    read_float(*found, "x", p.x, -8192.0f, 8192.0f, here, diag);
    read_float(*found, "y", p.y, -8192.0f, 8192.0f, here, diag);
    read_bool(*found, "percent", p.percent);
    read_enum(*found, "align", p.align, here, diag);
}

json::Value write(const Fade& f) {
    json::Object o;
    o.emplace_back("in_ms", json::Value(f.in_ms));
    o.emplace_back("out_ms", json::Value(f.out_ms));
    o.emplace_back("hold_ms", json::Value(f.hold_ms));
    o.emplace_back("start_opacity", json::Value(f.start_opacity));
    o.emplace_back("end_opacity", json::Value(f.end_opacity));
    o.emplace_back("easing", json::Value(to_string(f.easing)));
    return json::Value(std::move(o));
}

void read(const json::Value& v, std::string_view key, Fade& f, std::string_view path,
          ConfigDiagnostics& diag) {
    const json::Value* found = v.find(key);
    if (found == nullptr || !found->is_object()) return;
    const std::string here = join(path, key);
    read_int(*found, "in_ms", f.in_ms, 0, 10000, here, diag);
    read_int(*found, "out_ms", f.out_ms, 0, 10000, here, diag);
    read_int(*found, "hold_ms", f.hold_ms, 0, 600000, here, diag);
    read_float(*found, "start_opacity", f.start_opacity, 0.0f, 1.0f, here, diag);
    read_float(*found, "end_opacity", f.end_opacity, 0.0f, 1.0f, here, diag);
    read_enum(*found, "easing", f.easing, here, diag);
}

json::Value write(const Motion& m) {
    json::Object o;
    o.emplace_back("kind", json::Value(to_string(m.kind)));
    o.emplace_back("distance", json::Value(m.distance));
    o.emplace_back("scale_from", json::Value(m.scale_from));
    o.emplace_back("in_easing", json::Value(to_string(m.in_easing)));
    o.emplace_back("out_easing", json::Value(to_string(m.out_easing)));
    o.emplace_back("exit_slides", json::Value(m.exit_slides));
    return json::Value(std::move(o));
}

void read(const json::Value& v, std::string_view key, Motion& m, std::string_view path,
          ConfigDiagnostics& diag) {
    const json::Value* found = v.find(key);
    if (found == nullptr || !found->is_object()) return;
    const std::string here = join(path, key);
    read_enum(*found, "kind", m.kind, here, diag);
    read_float(*found, "distance", m.distance, 0.0f, 4096.0f, here, diag);
    read_float(*found, "scale_from", m.scale_from, 0.1f, 2.0f, here, diag);
    read_enum(*found, "in_easing", m.in_easing, here, diag);
    read_enum(*found, "out_easing", m.out_easing, here, diag);
    read_bool(*found, "exit_slides", m.exit_slides);
}

json::Value write(const NotificationStyle& s) {
    json::Object o;
    o.emplace_back("enabled", json::Value(s.enabled));
    o.emplace_back("title_format", json::Value(s.title_format));
    o.emplace_back("detail_format", json::Value(s.detail_format));
    o.emplace_back("icon", json::Value(to_string(s.icon)));
    o.emplace_back("accent", json::Value(s.accent.to_hex()));
    o.emplace_back("override_title_color", json::Value(s.override_title_color));
    o.emplace_back("title_color", json::Value(s.title_color.to_hex()));
    o.emplace_back("override_detail_color", json::Value(s.override_detail_color));
    o.emplace_back("detail_color", json::Value(s.detail_color.to_hex()));
    o.emplace_back("color_placeholders", json::Value(s.color_placeholders));
    o.emplace_back("placeholder_color", json::Value(s.placeholder_color.to_hex()));
    o.emplace_back("fade", write(s.fade));
    o.emplace_back("override_motion", json::Value(s.override_motion));
    o.emplace_back("motion", write(s.motion));
    o.emplace_back("sound", json::Value(s.sound));
    o.emplace_back("sound_file", json::Value(s.sound_file));
    o.emplace_back("wrap", json::Value(s.wrap));
    o.emplace_back("max_lines", json::Value(s.max_lines));
    o.emplace_back("priority", json::Value(s.priority));
    return json::Value(std::move(o));
}

void read(const json::Value& v, std::string_view key, NotificationStyle& s,
          std::string_view path, ConfigDiagnostics& diag) {
    const json::Value* found = v.find(key);
    if (found == nullptr || !found->is_object()) return;
    const std::string here = join(path, key);
    read_bool(*found, "enabled", s.enabled);
    read_string(*found, "title_format", s.title_format);
    read_string(*found, "detail_format", s.detail_format);
    read_enum(*found, "icon", s.icon, here, diag);
    read_color(*found, "accent", s.accent, here, diag);
    read_bool(*found, "override_title_color", s.override_title_color);
    read_color(*found, "title_color", s.title_color, here, diag);
    read_bool(*found, "override_detail_color", s.override_detail_color);
    read_color(*found, "detail_color", s.detail_color, here, diag);
    read_bool(*found, "color_placeholders", s.color_placeholders);
    read_color(*found, "placeholder_color", s.placeholder_color, here, diag);
    read(*found, "fade", s.fade, here, diag);
    read_bool(*found, "override_motion", s.override_motion);
    read(*found, "motion", s.motion, here, diag);
    read_bool(*found, "sound", s.sound);
    read_string(*found, "sound_file", s.sound_file);
    read_bool(*found, "wrap", s.wrap);
    read_int(*found, "max_lines", s.max_lines, 1, 8, here, diag);
    read_int(*found, "priority", s.priority, -100, 100, here, diag);
}

json::Value write(const NotificationBoxStyle& b) {
    json::Object o;
    o.emplace_back("background", json::Value(b.background.to_hex()));
    o.emplace_back("background_gradient", json::Value(b.background_gradient.to_hex()));
    o.emplace_back("corner_radius", json::Value(b.corner_radius));
    o.emplace_back("border", json::Value(to_string(b.border)));
    o.emplace_back("border_color", json::Value(b.border_color.to_hex()));
    o.emplace_back("border_thickness", json::Value(b.border_thickness));
    o.emplace_back("border_accent_opacity", json::Value(b.border_accent_opacity));
    o.emplace_back("accent_style", json::Value(to_string(b.accent_style)));
    o.emplace_back("accent_bar_width", json::Value(b.accent_bar_width));
    o.emplace_back("accent_tile_width", json::Value(b.accent_tile_width));
    o.emplace_back("accent_tile_opacity", json::Value(b.accent_tile_opacity));
    o.emplace_back("tile_icon_color", json::Value(b.tile_icon_color.to_hex()));
    o.emplace_back("shadow", json::Value(b.shadow));
    o.emplace_back("shadow_color", json::Value(b.shadow_color.to_hex()));
    o.emplace_back("shadow_size", json::Value(b.shadow_size));
    o.emplace_back("shadow_offset_y", json::Value(b.shadow_offset_y));
    o.emplace_back("auto_width", json::Value(b.auto_width));
    o.emplace_back("max_width", json::Value(b.max_width));
    return json::Value(std::move(o));
}

void read(const json::Value& v, std::string_view key, NotificationBoxStyle& b,
          std::string_view path, ConfigDiagnostics& diag) {
    const json::Value* found = v.find(key);
    if (found == nullptr || !found->is_object()) return;
    const std::string here = join(path, key);
    read_color(*found, "background", b.background, here, diag);
    read_color(*found, "background_gradient", b.background_gradient, here, diag);
    read_float(*found, "corner_radius", b.corner_radius, 0.0f, 64.0f, here, diag);
    read_enum(*found, "border", b.border, here, diag);
    read_color(*found, "border_color", b.border_color, here, diag);
    read_float(*found, "border_thickness", b.border_thickness, 0.0f, 8.0f, here, diag);
    read_float(*found, "border_accent_opacity", b.border_accent_opacity, 0.0f, 1.0f, here, diag);
    read_enum(*found, "accent_style", b.accent_style, here, diag);
    read_float(*found, "accent_bar_width", b.accent_bar_width, 0.0f, 32.0f, here, diag);
    read_float(*found, "accent_tile_width", b.accent_tile_width, 0.0f, 256.0f, here, diag);
    read_float(*found, "accent_tile_opacity", b.accent_tile_opacity, 0.0f, 1.0f, here, diag);
    read_color(*found, "tile_icon_color", b.tile_icon_color, here, diag);
    read_bool(*found, "shadow", b.shadow);
    read_color(*found, "shadow_color", b.shadow_color, here, diag);
    read_float(*found, "shadow_size", b.shadow_size, 0.0f, 64.0f, here, diag);
    read_float(*found, "shadow_offset_y", b.shadow_offset_y, -32.0f, 32.0f, here, diag);
    read_bool(*found, "auto_width", b.auto_width);
    read_float(*found, "max_width", b.max_width, 80.0f, 4096.0f, here, diag);
}

// --- sections ---------------------------------------------------------------------------------

json::Value write(const GeneralConfig& g) {
    json::Object o;
    o.emplace_back("enabled", json::Value(g.enabled));
    o.emplace_back("show_when_obs_closed", json::Value(g.show_when_obs_closed));
    o.emplace_back("master_opacity", json::Value(g.master_opacity));
    o.emplace_back("scale", json::Value(g.scale));
    o.emplace_back("profile_name", json::Value(g.profile_name));
    o.emplace_back("auto_profile_by_executable", json::Value(g.auto_profile_by_executable));
    o.emplace_back("advanced_settings", json::Value(g.advanced_settings));
    return json::Value(std::move(o));
}

json::Value write(const AppearanceConfig& a) {
    json::Object o;
    o.emplace_back("font_file", json::Value(a.font_file));
    o.emplace_back("font_face_index", json::Value(a.font_face_index));
    o.emplace_back("title_weight", json::Value(a.title_weight));
    o.emplace_back("detail_weight", json::Value(a.detail_weight));
    o.emplace_back("title_size", json::Value(a.title_size));
    o.emplace_back("detail_size", json::Value(a.detail_size));
    o.emplace_back("line_gap", json::Value(a.line_gap));
    o.emplace_back("icon_size", json::Value(a.icon_size));
    o.emplace_back("title_color", json::Value(a.title_color.to_hex()));
    o.emplace_back("detail_color", json::Value(a.detail_color.to_hex()));
    o.emplace_back("accent", json::Value(a.accent.to_hex()));
    o.emplace_back("text_shadow", json::Value(a.text_shadow));
    o.emplace_back("text_shadow_color", json::Value(a.text_shadow_color.to_hex()));
    o.emplace_back("text_shadow_offset", json::Value(a.text_shadow_offset));
    o.emplace_back("text_outline", json::Value(a.text_outline));
    o.emplace_back("text_outline_color", json::Value(a.text_outline_color.to_hex()));
    o.emplace_back("text_outline_thickness", json::Value(a.text_outline_thickness));
    return json::Value(std::move(o));
}

json::Value write(const NotificationsConfig& n) {
    json::Object o;
    o.emplace_back("placement", write(n.placement));
    o.emplace_back("max_visible", json::Value(n.max_visible));
    o.emplace_back("stack", json::Value(to_string(n.stack)));
    o.emplace_back("spacing", json::Value(n.spacing));
    o.emplace_back("width", json::Value(n.width));
    o.emplace_back("min_height", json::Value(n.min_height));
    o.emplace_back("box", write(n.box));
    o.emplace_back("padding_x", json::Value(n.padding_x));
    o.emplace_back("padding_y", json::Value(n.padding_y));
    o.emplace_back("icon_gap", json::Value(n.icon_gap));
    o.emplace_back("font_scale", json::Value(n.font_scale));
    o.emplace_back("motion", write(n.motion));
    o.emplace_back("merge_duplicates", json::Value(n.merge_duplicates));
    o.emplace_back("suppress_after_connect_ms", json::Value(n.suppress_after_connect_ms));
    o.emplace_back("min_interval_ms", json::Value(n.min_interval_ms));
    for (const CategoryEntry& entry : categories()) {
        o.emplace_back(entry.key, write(n.*(entry.member)));
    }
    return json::Value(std::move(o));
}

json::Value write(const StatusConfig& s) {
    json::Object o;
    o.emplace_back("placement", write(s.placement));
    o.emplace_back("show_while_recording", json::Value(s.show_while_recording));
    o.emplace_back("show_while_streaming", json::Value(s.show_while_streaming));
    o.emplace_back("show_while_replay_armed", json::Value(s.show_while_replay_armed));
    o.emplace_back("show_timer", json::Value(s.show_timer));
    o.emplace_back("pulse", json::Value(s.pulse));
    o.emplace_back("pulse_hz", json::Value(s.pulse_hz));
    o.emplace_back("pulse_depth", json::Value(s.pulse_depth));
    o.emplace_back("dot_size", json::Value(s.dot_size));
    o.emplace_back("font_size", json::Value(s.font_size));
    o.emplace_back("padding_x", json::Value(s.padding_x));
    o.emplace_back("padding_y", json::Value(s.padding_y));
    o.emplace_back("corner_radius", json::Value(s.corner_radius));
    o.emplace_back("show_background", json::Value(s.show_background));
    o.emplace_back("background", json::Value(s.background.to_hex()));
    o.emplace_back("text", json::Value(s.text.to_hex()));
    o.emplace_back("recording_color", json::Value(s.recording_color.to_hex()));
    o.emplace_back("streaming_color", json::Value(s.streaming_color.to_hex()));
    o.emplace_back("paused_color", json::Value(s.paused_color.to_hex()));
    o.emplace_back("fade", write(s.fade));
    return json::Value(std::move(o));
}

json::Value write(const AnimationConfig& a) {
    json::Object o;
    o.emplace_back("enabled", json::Value(a.enabled));
    o.emplace_back("animate_restack", json::Value(a.animate_restack));
    o.emplace_back("restack_ms", json::Value(a.restack_ms));
    o.emplace_back("restack_easing", json::Value(to_string(a.restack_easing)));
    return json::Value(std::move(o));
}

json::Value write(const IntegrationConfig& i) {
    json::Object o;
    o.emplace_back("pipe_name", json::Value(i.pipe_name));
    o.emplace_back("reconnect_initial_ms", json::Value(i.reconnect_initial_ms));
    o.emplace_back("reconnect_max_ms", json::Value(i.reconnect_max_ms));
    o.emplace_back("stale_after_ms", json::Value(i.stale_after_ms));
    o.emplace_back("ping_interval_ms", json::Value(i.ping_interval_ms));
    o.emplace_back("auto_connect", json::Value(i.auto_connect));
    return json::Value(std::move(o));
}

json::Value write(const LoggingConfig& l) {
    json::Object o;
    o.emplace_back("level", json::Value(l.level));
    o.emplace_back("to_file", json::Value(l.to_file));
    o.emplace_back("file_name", json::Value(l.file_name));
    o.emplace_back("max_file_kb", json::Value(l.max_file_kb));
    o.emplace_back("include_paths", json::Value(l.include_paths));
    o.emplace_back("show_diagnostics_overlay", json::Value(l.show_diagnostics_overlay));
    return json::Value(std::move(o));
}

/// Keys the loader consumed. Anything outside this set is round-tripped untouched, which is
/// what lets a profile written by a newer build survive a trip through an older one.
bool is_known_top_level(std::string_view key) {
    static constexpr std::string_view known[] = {
        "config_version", "general", "appearance", "notifications",
        "status", "animation", "integration", "logging",
    };
    return std::find(std::begin(known), std::end(known), key) != std::end(known);
}

}  // namespace

json::Value Config::to_json() const {
    json::Object o;
    o.emplace_back("config_version", json::Value(config_version));
    o.emplace_back("general", write(general));
    o.emplace_back("appearance", write(appearance));
    o.emplace_back("notifications", write(notifications));
    o.emplace_back("status", write(status));
    o.emplace_back("animation", write(animation));
    o.emplace_back("integration", write(integration));
    o.emplace_back("logging", write(logging));
    // Preserved unknowns go last so a diff against a known-good file stays readable.
    for (const json::Member& member : unknown) o.emplace_back(member.first, member.second);
    return json::Value(std::move(o));
}

Config Config::from_json(const json::Value& doc, ConfigDiagnostics& diag) {
    Config c = Config::defaults();
    if (!doc.is_object()) {
        diag.add(Sev::Error, "", "document is not an object; using defaults");
        diag.from_defaults = true;
        return c;
    }

    c.config_version = static_cast<int>(doc.get_int("config_version", kConfigVersion));
    diag.loaded_version = c.config_version;
    if (c.config_version > kConfigVersion) {
        diag.newer_than_supported = true;
        diag.add(Sev::Warning, "config_version",
                 "written by a newer build; unknown settings are preserved but not applied");
    }

    if (const json::Value* g = doc.find("general"); g != nullptr && g->is_object()) {
        read_bool(*g, "enabled", c.general.enabled);
        read_bool(*g, "show_when_obs_closed", c.general.show_when_obs_closed);
        read_float(*g, "master_opacity", c.general.master_opacity, 0.0f, 1.0f, "general", diag);
        read_float(*g, "scale", c.general.scale, 0.25f, 4.0f, "general", diag);
        read_string(*g, "profile_name", c.general.profile_name);
        read_bool(*g, "auto_profile_by_executable", c.general.auto_profile_by_executable);
        read_bool(*g, "advanced_settings", c.general.advanced_settings);
    }

    if (const json::Value* a = doc.find("appearance"); a != nullptr && a->is_object()) {
        read_string(*a, "font_file", c.appearance.font_file);
        read_int(*a, "font_face_index", c.appearance.font_face_index, 0, 64, "appearance", diag);
        read_int(*a, "title_weight", c.appearance.title_weight, 100, 900, "appearance", diag);
        read_int(*a, "detail_weight", c.appearance.detail_weight, 100, 900, "appearance", diag);
        read_float(*a, "title_size", c.appearance.title_size, 6.0f, 96.0f, "appearance", diag);
        read_float(*a, "detail_size", c.appearance.detail_size, 6.0f, 96.0f, "appearance", diag);
        read_float(*a, "line_gap", c.appearance.line_gap, -8.0f, 32.0f, "appearance", diag);
        read_float(*a, "icon_size", c.appearance.icon_size, 0.0f, 128.0f, "appearance", diag);
        read_color(*a, "title_color", c.appearance.title_color, "appearance", diag);
        read_color(*a, "detail_color", c.appearance.detail_color, "appearance", diag);
        read_color(*a, "accent", c.appearance.accent, "appearance", diag);
        read_bool(*a, "text_shadow", c.appearance.text_shadow);
        read_color(*a, "text_shadow_color", c.appearance.text_shadow_color, "appearance", diag);
        read_float(*a, "text_shadow_offset", c.appearance.text_shadow_offset, 0.0f, 8.0f,
                   "appearance", diag);
        read_bool(*a, "text_outline", c.appearance.text_outline);
        read_color(*a, "text_outline_color", c.appearance.text_outline_color, "appearance", diag);
        read_float(*a, "text_outline_thickness", c.appearance.text_outline_thickness, 0.0f, 6.0f,
                   "appearance", diag);
    }

    if (const json::Value* n = doc.find("notifications"); n != nullptr && n->is_object()) {
        NotificationsConfig& nc = c.notifications;
        read(*n, "placement", nc.placement, "notifications", diag);
        read_int(*n, "max_visible", nc.max_visible, 1, 32, "notifications", diag);
        read_enum(*n, "stack", nc.stack, "notifications", diag);
        read_float(*n, "spacing", nc.spacing, 0.0f, 128.0f, "notifications", diag);
        read_float(*n, "width", nc.width, 64.0f, 4096.0f, "notifications", diag);
        read_float(*n, "min_height", nc.min_height, 8.0f, 512.0f, "notifications", diag);
        read(*n, "box", nc.box, "notifications", diag);
        read_float(*n, "padding_x", nc.padding_x, 0.0f, 128.0f, "notifications", diag);
        read_float(*n, "padding_y", nc.padding_y, 0.0f, 128.0f, "notifications", diag);
        read_float(*n, "icon_gap", nc.icon_gap, 0.0f, 128.0f, "notifications", diag);
        read_float(*n, "font_scale", nc.font_scale, 0.25f, 4.0f, "notifications", diag);
        read(*n, "motion", nc.motion, "notifications", diag);
        read_bool(*n, "merge_duplicates", nc.merge_duplicates);
        read_int(*n, "suppress_after_connect_ms", nc.suppress_after_connect_ms, 0, 60000,
                 "notifications", diag);
        read_int(*n, "min_interval_ms", nc.min_interval_ms, 0, 60000, "notifications", diag);
        for (const CategoryEntry& entry : categories()) {
            read(*n, entry.key, nc.*(entry.member), "notifications", diag);
        }
    }

    if (const json::Value* s = doc.find("status"); s != nullptr && s->is_object()) {
        StatusConfig& sc = c.status;
        read(*s, "placement", sc.placement, "status", diag);
        read_bool(*s, "show_while_recording", sc.show_while_recording);
        read_bool(*s, "show_while_streaming", sc.show_while_streaming);
        read_bool(*s, "show_while_replay_armed", sc.show_while_replay_armed);
        read_bool(*s, "show_timer", sc.show_timer);
        read_bool(*s, "pulse", sc.pulse);
        read_float(*s, "pulse_hz", sc.pulse_hz, 0.0f, 10.0f, "status", diag);
        read_float(*s, "pulse_depth", sc.pulse_depth, 0.0f, 1.0f, "status", diag);
        read_float(*s, "dot_size", sc.dot_size, 0.0f, 64.0f, "status", diag);
        read_float(*s, "font_size", sc.font_size, 6.0f, 96.0f, "status", diag);
        read_float(*s, "padding_x", sc.padding_x, 0.0f, 64.0f, "status", diag);
        read_float(*s, "padding_y", sc.padding_y, 0.0f, 64.0f, "status", diag);
        read_float(*s, "corner_radius", sc.corner_radius, 0.0f, 64.0f, "status", diag);
        read_bool(*s, "show_background", sc.show_background);
        read_color(*s, "background", sc.background, "status", diag);
        read_color(*s, "text", sc.text, "status", diag);
        read_color(*s, "recording_color", sc.recording_color, "status", diag);
        read_color(*s, "streaming_color", sc.streaming_color, "status", diag);
        read_color(*s, "paused_color", sc.paused_color, "status", diag);
        read(*s, "fade", sc.fade, "status", diag);
    }

    if (const json::Value* a = doc.find("animation"); a != nullptr && a->is_object()) {
        read_bool(*a, "enabled", c.animation.enabled);
        read_bool(*a, "animate_restack", c.animation.animate_restack);
        read_int(*a, "restack_ms", c.animation.restack_ms, 0, 2000, "animation", diag);
        read_enum(*a, "restack_easing", c.animation.restack_easing, "animation", diag);
    }

    if (const json::Value* i = doc.find("integration"); i != nullptr && i->is_object()) {
        read_string(*i, "pipe_name", c.integration.pipe_name);
        read_int(*i, "reconnect_initial_ms", c.integration.reconnect_initial_ms, 50, 60000,
                 "integration", diag);
        read_int(*i, "reconnect_max_ms", c.integration.reconnect_max_ms, 50, 300000,
                 "integration", diag);
        read_int(*i, "stale_after_ms", c.integration.stale_after_ms, 500, 600000,
                 "integration", diag);
        read_int(*i, "ping_interval_ms", c.integration.ping_interval_ms, 500, 600000,
                 "integration", diag);
        read_bool(*i, "auto_connect", c.integration.auto_connect);
    }

    if (const json::Value* l = doc.find("logging"); l != nullptr && l->is_object()) {
        read_string(*l, "level", c.logging.level);
        read_bool(*l, "to_file", c.logging.to_file);
        read_string(*l, "file_name", c.logging.file_name);
        read_int(*l, "max_file_kb", c.logging.max_file_kb, 16, 65536, "logging", diag);
        read_bool(*l, "include_paths", c.logging.include_paths);
        read_bool(*l, "show_diagnostics_overlay", c.logging.show_diagnostics_overlay);
    }

    for (const json::Member& member : doc.as_object()) {
        if (!is_known_top_level(member.first)) c.unknown.emplace_back(member);
    }

    c.clamp(diag);
    return c;
}

Config Config::parse(std::string_view text, ConfigDiagnostics& diag) {
    // The limits here are the JSON parser's, not the protocol's: a profile is a file the user
    // owns and may legitimately be larger than a wire message, but it is still bounded so a
    // corrupt file cannot exhaust memory.
    json::Limits limits;
    limits.max_total_bytes = 1 << 20;
    limits.max_object_members = 512;
    limits.max_array_elements = 4096;

    json::ParseResult parsed = json::parse(text, limits);
    if (!parsed.ok) {
        diag.add(Sev::Error, "", "could not be parsed (" + parsed.error + "); using defaults");
        diag.from_defaults = true;
        return Config::defaults();
    }
    migrate(parsed.value, diag);
    return Config::from_json(parsed.value, diag);
}

std::string Config::serialise() const { return to_json().dump(); }

void Config::clamp(ConfigDiagnostics& diag) {
    // A percentage placement is a fraction of the viewport, so anything outside [-1, 2] puts the
    // element so far off screen that it can never be found again -- which is indistinguishable
    // from the overlay being broken.
    const auto clamp_placement = [&diag](Placement& p, const char* path) {
        if (!p.percent) return;
        const float x = std::clamp(p.x, -1.0f, 2.0f);
        const float y = std::clamp(p.y, -1.0f, 2.0f);
        if (x != p.x || y != p.y) {
            diag.add(Sev::Info, path, "percentage offset clamped into the viewport");
            p.x = x;
            p.y = y;
        }
    };
    clamp_placement(notifications.placement, "notifications.placement");
    clamp_placement(status.placement, "status.placement");

    if (integration.reconnect_max_ms < integration.reconnect_initial_ms) {
        diag.add(Sev::Info, "integration.reconnect_max_ms",
                 "below reconnect_initial_ms; raised to match");
        integration.reconnect_max_ms = integration.reconnect_initial_ms;
    }
    // A toast whose fade-in and fade-out together outlast nothing still needs a moment at full
    // opacity, or it is a flash rather than a notification.
    for (const CategoryEntry& entry : categories()) {
        NotificationStyle& s = notifications.*(entry.member);
        if (s.fade.in_ms + s.fade.hold_ms + s.fade.out_ms <= 0) {
            diag.add(Sev::Info, std::string("notifications.") + entry.key + ".fade",
                     "zero total lifetime; restored the default hold");
            s.fade.hold_ms = 2000;
        }
        if (s.max_lines < 1) s.max_lines = 1;
    }
    if (notifications.box.max_width < notifications.width) {
        diag.add(Sev::Info, "notifications.box.max_width",
                 "below notifications.width; raised to match");
        notifications.box.max_width = notifications.width;
    }
}

bool migrate(json::Value& doc, ConfigDiagnostics& diag) {
    if (!doc.is_object()) return false;
    const int version = static_cast<int>(doc.get_int("config_version", kConfigVersion));
    if (version >= kConfigVersion) return false;
    // v1 is the first release, so there is nothing to migrate *from* yet. The scaffolding is
    // here rather than added later because the shape of the step -- mutate in place, record it,
    // bump the version -- is what a later migration has to fit into, and retrofitting that onto
    // a loader that never had it is how silent data loss happens.
    doc.set("config_version", json::Value(kConfigVersion));
    diag.migrated = true;
    diag.add(Sev::Info, "config_version",
             "migrated from version " + std::to_string(version));
    return true;
}

}  // namespace obsn
