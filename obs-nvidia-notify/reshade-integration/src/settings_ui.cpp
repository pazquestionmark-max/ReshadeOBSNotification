// SPDX-License-Identifier: MIT
#include "settings_ui.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <imgui.h>
// reshade.hpp must follow imgui.h: it supplies the inline definitions for ImGui:: that route
// through ReShade's function table.
#include <reshade.hpp>

#include "icons.hpp"

namespace obsn::overlay {
namespace {

/// Every control routes through these wrappers rather than calling ImGui directly, because
/// every one of them has to report whether it changed something -- the add-on re-clamps and
/// re-hashes the configuration only when it did. Forgetting that on one control is a setting
/// that silently does not take effect, which is the hardest kind of bug to notice here.
bool checkbox(const char* label, bool& value, const char* tooltip = nullptr) {
    const bool changed = ImGui::Checkbox(label, &value);
    if (tooltip != nullptr && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return changed;
}

bool slider_float(const char* label, float& value, float lo, float hi, const char* format = "%.1f",
                  const char* tooltip = nullptr) {
    const bool changed = ImGui::SliderFloat(label, &value, lo, hi, format);
    if (tooltip != nullptr && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return changed;
}

bool slider_int(const char* label, int& value, int lo, int hi, const char* tooltip = nullptr) {
    const bool changed = ImGui::SliderInt(label, &value, lo, hi);
    if (tooltip != nullptr && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return changed;
}

bool color_edit(const char* label, Color& color) {
    float rgba[4] = {static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f,
                     static_cast<float>(color.b) / 255.0f, static_cast<float>(color.a) / 255.0f};
    if (!ImGui::ColorEdit4(label, rgba, ImGuiColorEditFlags_AlphaBar |
                                            ImGuiColorEditFlags_AlphaPreviewHalf)) {
        return false;
    }
    color.r = static_cast<std::uint8_t>(std::clamp(rgba[0] * 255.0f + 0.5f, 0.0f, 255.0f));
    color.g = static_cast<std::uint8_t>(std::clamp(rgba[1] * 255.0f + 0.5f, 0.0f, 255.0f));
    color.b = static_cast<std::uint8_t>(std::clamp(rgba[2] * 255.0f + 0.5f, 0.0f, 255.0f));
    color.a = static_cast<std::uint8_t>(std::clamp(rgba[3] * 255.0f + 0.5f, 0.0f, 255.0f));
    return true;
}

bool text_field(const char* label, std::string& value, std::size_t capacity = 256) {
    std::vector<char> buffer(std::max(capacity, value.size() + 1), '\0');
    std::memcpy(buffer.data(), value.data(), value.size());
    if (!ImGui::InputText(label, buffer.data(), buffer.size())) return false;
    value.assign(buffer.data());
    return true;
}

/// A combo built from an enum's own to_string/parse_enum pair, so the list in the UI cannot
/// drift from the list the serialiser accepts.
template <typename E, std::size_t N>
bool enum_combo(const char* label, E& value, const E (&values)[N]) {
    const char* current = to_string(value);
    if (!ImGui::BeginCombo(label, current)) return false;
    bool changed = false;
    for (const E candidate : values) {
        const bool selected = candidate == value;
        if (ImGui::Selectable(to_string(candidate), selected)) {
            value = candidate;
            changed = true;
        }
        if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
    return changed;
}

constexpr Anchor kAnchorValues[] = {
    Anchor::TopLeft, Anchor::TopCenter, Anchor::TopRight,
    Anchor::CenterLeft, Anchor::Center, Anchor::CenterRight,
    Anchor::BottomLeft, Anchor::BottomCenter, Anchor::BottomRight,
};
constexpr Align kAlignValues[] = {Align::Left, Align::Center, Align::Right};
constexpr StackDirection kStackValues[] = {StackDirection::Down, StackDirection::Up};
constexpr NotificationBorder kBorderValues[] = {
    NotificationBorder::None, NotificationBorder::Accent, NotificationBorder::Custom};
constexpr AccentStyle kAccentValues[] = {AccentStyle::None, AccentStyle::Bar, AccentStyle::Tile,
                                         AccentStyle::BarAndTile};
constexpr MotionKind kMotionValues[] = {MotionKind::None, MotionKind::SlideFromEdge,
                                        MotionKind::SlideHorizontal, MotionKind::SlideVertical,
                                        MotionKind::Scale};
constexpr Easing kEasingValues[] = {
    Easing::Linear, Easing::EaseIn, Easing::EaseOut, Easing::EaseInOut,
    Easing::EaseInCubic, Easing::EaseOutCubic, Easing::EaseInOutCubic,
    Easing::EaseOutQuint, Easing::EaseOutExpo, Easing::EaseOutBack, Easing::EaseOutElastic,
};
constexpr IconShape kIconValues[] = {
    IconShape::None, IconShape::Record, IconShape::RecordRing, IconShape::Stop, IconShape::Pause,
    IconShape::Play, IconShape::Save, IconShape::Replay, IconShape::Clock, IconShape::Camera,
    IconShape::Broadcast, IconShape::Film, IconShape::Scissors, IconShape::Warning,
    IconShape::Check, IconShape::Cross, IconShape::Folder, IconShape::Layers, IconShape::Disk,
    IconShape::Dot, IconShape::Circle, IconShape::Ring, IconShape::Square, IconShape::Diamond,
    IconShape::Triangle, IconShape::Star, IconShape::Chevron,
};

bool placement_editor(const char* id, Placement& placement) {
    ImGui::PushID(id);
    bool changed = false;
    changed |= checkbox("Visible", placement.visible);
    changed |= enum_combo("Corner", placement.anchor, kAnchorValues);
    changed |= checkbox("Offsets are percentages", placement.percent,
                        "Pixel offsets keep the same margin at every resolution; percentages "
                        "keep the same proportion.");
    const float limit = placement.percent ? 1.0f : 400.0f;
    changed |= slider_float("Offset X", placement.x, placement.percent ? -0.5f : -200.0f, limit);
    changed |= slider_float("Offset Y", placement.y, placement.percent ? -0.5f : -200.0f, limit);
    changed |= enum_combo("Align", placement.align, kAlignValues);
    ImGui::PopID();
    return changed;
}

bool fade_editor(const char* id, Fade& fade) {
    ImGui::PushID(id);
    bool changed = false;
    changed |= slider_int("Fade in (ms)", fade.in_ms, 0, 2000);
    changed |= slider_int("Hold (ms)", fade.hold_ms, 0, 30000);
    changed |= slider_int("Fade out (ms)", fade.out_ms, 0, 2000);
    changed |= enum_combo("Fade easing", fade.easing, kEasingValues);
    ImGui::PopID();
    return changed;
}

bool motion_editor(const char* id, Motion& motion) {
    ImGui::PushID(id);
    bool changed = false;
    changed |= enum_combo("Motion", motion.kind, kMotionValues);
    if (motion.kind == MotionKind::Scale) {
        changed |= slider_float("Start scale", motion.scale_from, 0.2f, 1.5f, "%.2f");
    } else if (motion.kind != MotionKind::None) {
        changed |= slider_float("Distance (px)", motion.distance, 0.0f, 600.0f, "%.0f",
                                "0 travels the toast's own width, so it starts fully outside "
                                "the screen edge.");
    }
    changed |= enum_combo("Entry easing", motion.in_easing, kEasingValues);
    changed |= enum_combo("Exit easing", motion.out_easing, kEasingValues);
    changed |= checkbox("Slide back out", motion.exit_slides,
                        "Off leaves the toast in place and only fades it away.");
    ImGui::PopID();
    return changed;
}

void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

}  // namespace

void SettingsUi::refresh_profiles(const ProfileStore& profiles) { profiles_ = profiles.list(); }

void SettingsUi::set_status(std::string message, bool is_error) {
    status_message_ = std::move(message);
    status_is_error_ = is_error;
}

void SettingsUi::draw_category(const CategoryEntry& entry, Config& config,
                               SettingsActions& actions) {
    NotificationStyle& style = config.notifications.*(entry.member);
    ImGui::PushID(entry.key);

    // The enable switch sits outside the collapsing header, so turning a category off never
    // requires opening it first.
    if (checkbox("##enabled", style.enabled)) actions.config_changed = true;
    ImGui::SameLine();

    // A swatch in the category's own accent, so the list reads as the toasts do.
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight() * 0.6f;
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(cursor.x, cursor.y + height * 0.3f),
        ImVec2(cursor.x + height * 0.45f, cursor.y + height * 1.3f), style.accent.to_abgr(), 1.0f);
    ImGui::Dummy(ImVec2(height * 0.45f + 6.0f, height));
    ImGui::SameLine();

    const bool open = ImGui::CollapsingHeader(entry.label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 46.0f);
    if (ImGui::SmallButton("Test")) actions.test_notification = entry.kind;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Raise one sample toast of this category, using the real formatter.");
    }

    if (open) {
        ImGui::Indent();
        bool changed = false;
        changed |= text_field("Title", style.title_format);
        changed |= text_field("Detail", style.detail_format);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Leave empty for a single-line toast.");
        }
        ImGui::TextDisabled("{file} {path} {folder} {duration} {size} {scene} {previous_scene}");
        ImGui::TextDisabled("{profile} {service} {reason} {detail} {time} {attempt} {elapsed}");

        changed |= enum_combo("Icon", style.icon, kIconValues);
        changed |= color_edit("Accent", style.accent);
        help_marker("Used for the tile, the bar and the accent border. This one colour is what "
                    "gives the category its identity.");

        changed |= checkbox("Override title colour", style.override_title_color);
        if (style.override_title_color) changed |= color_edit("Title colour", style.title_color);
        changed |= checkbox("Override detail colour", style.override_detail_color);
        if (style.override_detail_color) {
            changed |= color_edit("Detail colour", style.detail_color);
        }
        changed |= checkbox("Colour placeholders", style.color_placeholders);
        if (style.color_placeholders) {
            changed |= color_edit("Placeholder colour", style.placeholder_color);
        }

        changed |= fade_editor("fade", style.fade);
        changed |= checkbox("Override motion", style.override_motion,
                            "Off uses the shared motion from the Notifications tab.");
        if (style.override_motion) changed |= motion_editor("motion", style.motion);

        changed |= checkbox("Wrap the detail line", style.wrap);
        if (style.wrap) changed |= slider_int("Max lines", style.max_lines, 1, 8);
        changed |= slider_int("Priority", style.priority, -10, 10);
        help_marker("When more toasts arrive than can be shown, the lowest priority is dropped "
                    "first.");

        if (config.general.advanced_settings) {
            changed |= checkbox("Play a sound", style.sound);
            if (style.sound) changed |= text_field("Sound file", style.sound_file);
        }

        if (changed) actions.config_changed = true;
        ImGui::Unindent();
    }
    ImGui::PopID();
}

void SettingsUi::draw_notifications_tab(Config& config, SettingsActions& actions) {
    NotificationsConfig& nc = config.notifications;
    bool changed = false;

    if (ImGui::CollapsingHeader("Placement and stack", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= placement_editor("notifications", nc.placement);
        changed |= enum_combo("Stack direction", nc.stack, kStackValues);
        changed |= slider_int("Most on screen at once", nc.max_visible, 1, 12);
        changed |= slider_float("Spacing", nc.spacing, 0.0f, 48.0f, "%.0f");
        changed |= slider_float("Minimum width", nc.width, 80.0f, 900.0f, "%.0f");
        changed |= slider_float("Minimum height", nc.min_height, 16.0f, 200.0f, "%.0f");
    }

    if (ImGui::CollapsingHeader("Box", ImGuiTreeNodeFlags_DefaultOpen)) {
        NotificationBoxStyle& box = nc.box;
        changed |= color_edit("Background", box.background);
        changed |= slider_float("Corner radius", box.corner_radius, 0.0f, 32.0f, "%.0f");
        changed |= enum_combo("Accent style", box.accent_style, kAccentValues);
        if (box.accent_style == AccentStyle::Tile ||
            box.accent_style == AccentStyle::BarAndTile) {
            changed |= slider_float("Tile width", box.accent_tile_width, 16.0f, 160.0f, "%.0f");
            changed |= slider_float("Tile opacity", box.accent_tile_opacity, 0.0f, 1.0f, "%.2f");
            changed |= color_edit("Icon on tile", box.tile_icon_color);
        }
        if (box.accent_style == AccentStyle::Bar || box.accent_style == AccentStyle::BarAndTile) {
            changed |= slider_float("Bar width", box.accent_bar_width, 1.0f, 16.0f, "%.0f");
        }
        changed |= enum_combo("Border", box.border, kBorderValues);
        if (box.border == NotificationBorder::Custom) {
            changed |= color_edit("Border colour", box.border_color);
        }
        if (box.border != NotificationBorder::None) {
            changed |= slider_float("Border thickness", box.border_thickness, 0.0f, 6.0f, "%.1f");
        }
        if (box.border == NotificationBorder::Accent) {
            changed |= slider_float("Border strength", box.border_accent_opacity, 0.0f, 1.0f,
                                    "%.2f");
        }
        changed |= checkbox("Drop shadow", box.shadow);
        if (box.shadow) {
            changed |= color_edit("Shadow colour", box.shadow_color);
            changed |= slider_float("Shadow size", box.shadow_size, 0.0f, 32.0f, "%.0f");
            changed |= slider_float("Shadow offset", box.shadow_offset_y, -16.0f, 16.0f, "%.0f");
        }
        changed |= checkbox("Size each toast to its text", box.auto_width);
        changed |= slider_float("Maximum width", box.max_width, 120.0f, 1600.0f, "%.0f");
        changed |= slider_float("Padding X", nc.padding_x, 0.0f, 48.0f, "%.0f");
        changed |= slider_float("Padding Y", nc.padding_y, 0.0f, 48.0f, "%.0f");
        changed |= slider_float("Icon gap", nc.icon_gap, 0.0f, 48.0f, "%.0f");
        changed |= slider_float("Text scale", nc.font_scale, 0.5f, 2.5f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Motion", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= motion_editor("shared_motion", nc.motion);
        ImGui::Separator();
        changed |= checkbox("Merge repeats", nc.merge_duplicates,
                            "An identical toast restarts rather than stacking a second copy.");
        changed |= slider_int("Minimum gap per category (ms)", nc.min_interval_ms, 0, 5000);
        help_marker("Two events from the same category arriving inside this window replace each "
                    "other in place instead of stacking.");
        changed |= slider_int("Ignore events just after connecting (ms)",
                              nc.suppress_after_connect_ms, 0, 10000);
        help_marker("State that was already true when the overlay attached is not news. Raise "
                    "this if attaching mid-session announces things that were already running.");
    }

    ImGui::Separator();
    if (ImGui::Button("Show every category")) actions.seed_preview = true;
    ImGui::SameLine();
    ImGui::TextDisabled("raises one sample toast of each enabled category");

    if (ImGui::CollapsingHeader("Categories", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const CategoryEntry& entry : categories()) {
            draw_category(entry, config, actions);
        }
    }

    if (changed) actions.config_changed = true;
}

void SettingsUi::draw_appearance_tab(Config& config, SettingsActions& actions) {
    AppearanceConfig& a = config.appearance;
    bool changed = false;

    changed |= slider_float("Overall scale", config.general.scale, 0.5f, 2.5f, "%.2f");
    changed |= slider_float("Overall opacity", config.general.master_opacity, 0.1f, 1.0f, "%.2f");

    ImGui::Separator();
    ImGui::TextUnformatted("Typeface");
    if (fonts_ != nullptr) {
        const std::vector<FontFile>& available = fonts_->available();
        const std::string current = a.font_file.empty() ? "ReShade's own font" : a.font_file;
        if (ImGui::BeginCombo("Font", current.c_str())) {
            if (ImGui::Selectable("ReShade's own font", a.font_file.empty())) {
                a.font_file.clear();
                a.font_face_index = 0;
                changed = true;
            }
            for (const FontFile& font : available) {
                const bool selected = font.file == a.font_file &&
                                      font.face_index == a.font_face_index;
                if (ImGui::Selectable(font.label().c_str(), selected)) {
                    a.font_file = font.file;
                    a.font_face_index = font.face_index;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Rescan fonts")) fonts_->rescan();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", fonts_->status().c_str());
    } else {
        changed |= text_field("Font file", a.font_file);
    }

    changed |= slider_float("Title size", a.title_size, 8.0f, 40.0f, "%.0f");
    changed |= slider_int("Title weight", a.title_weight, 100, 900);
    changed |= slider_float("Detail size", a.detail_size, 8.0f, 40.0f, "%.0f");
    changed |= slider_int("Detail weight", a.detail_weight, 100, 900);
    changed |= slider_float("Line gap", a.line_gap, -4.0f, 20.0f, "%.0f");
    changed |= slider_float("Icon size", a.icon_size, 0.0f, 64.0f, "%.0f");

    ImGui::Separator();
    changed |= color_edit("Title colour", a.title_color);
    changed |= color_edit("Detail colour", a.detail_color);
    changed |= color_edit("House accent", a.accent);
    help_marker("The colour a category inherits when it has no accent of its own, and the "
                "colour the status indicator uses for an armed replay buffer.");

    ImGui::Separator();
    changed |= checkbox("Text shadow", a.text_shadow);
    if (a.text_shadow) {
        changed |= color_edit("Shadow colour", a.text_shadow_color);
        changed |= slider_float("Shadow offset", a.text_shadow_offset, 0.0f, 4.0f, "%.1f");
    }
    changed |= checkbox("Text outline", a.text_outline,
                        "Costs eight extra draws per line, and is the only thing that stays "
                        "readable over any background rather than most of them.");
    if (a.text_outline) {
        changed |= color_edit("Outline colour", a.text_outline_color);
        changed |= slider_float("Outline thickness", a.text_outline_thickness, 0.0f, 4.0f, "%.1f");
    }

    ImGui::Separator();
    changed |= checkbox("Animate", config.animation.enabled,
                        "Off pins every toast in place and uses a plain fade.");
    if (config.animation.enabled) {
        changed |= checkbox("Animate the stack closing up", config.animation.animate_restack);
        if (config.animation.animate_restack) {
            changed |= slider_int("Restack (ms)", config.animation.restack_ms, 0, 800);
            changed |= enum_combo("Restack easing", config.animation.restack_easing,
                                  kEasingValues);
        }
    }

    if (changed) actions.config_changed = true;
}

void SettingsUi::draw_status_tab(Config& config, SettingsActions& actions) {
    StatusConfig& s = config.status;
    bool changed = false;

    ImGui::TextWrapped(
        "A small mark that stays on screen for as long as something is being captured. This is "
        "a different job from a notification: it answers \"am I still recording?\" at any "
        "moment rather than announcing a change. It is off by default because it is "
        "permanently on screen.");
    ImGui::Separator();

    changed |= placement_editor("status", s.placement);
    ImGui::Separator();
    changed |= checkbox("While recording", s.show_while_recording);
    changed |= checkbox("While streaming", s.show_while_streaming);
    changed |= checkbox("While the replay buffer is armed", s.show_while_replay_armed,
                        "The buffer is usually armed for a whole session, so a permanent mark "
                        "for it is mostly noise.");
    changed |= checkbox("Show the elapsed timer", s.show_timer);
    changed |= checkbox("Pulse", s.pulse);
    if (s.pulse) {
        changed |= slider_float("Pulse rate (Hz)", s.pulse_hz, 0.1f, 4.0f, "%.2f");
        changed |= slider_float("Pulse depth", s.pulse_depth, 0.0f, 1.0f, "%.2f");
    }
    changed |= slider_float("Dot size", s.dot_size, 0.0f, 32.0f, "%.0f");
    changed |= slider_float("Text size", s.font_size, 8.0f, 40.0f, "%.0f");
    changed |= checkbox("Background", s.show_background);
    if (s.show_background) {
        changed |= color_edit("Background colour", s.background);
        changed |= slider_float("Corner radius", s.corner_radius, 0.0f, 24.0f, "%.0f");
        changed |= slider_float("Padding X", s.padding_x, 0.0f, 32.0f, "%.0f");
        changed |= slider_float("Padding Y", s.padding_y, 0.0f, 32.0f, "%.0f");
    }
    changed |= color_edit("Text colour", s.text);
    changed |= color_edit("Recording", s.recording_color);
    changed |= color_edit("Streaming", s.streaming_color);
    changed |= color_edit("Paused", s.paused_color);

    if (changed) actions.config_changed = true;
}

void SettingsUi::draw_profiles_tab(Config& config, ProfileStore& profiles,
                                   SettingsActions& actions) {
    ImGui::TextDisabled("%s", profiles.root().c_str());
    ImGui::Separator();

    for (const ProfileInfo& profile : profiles_) {
        ImGui::PushID(profile.name.c_str());
        const bool current = profile.name == config.general.profile_name;
        if (ImGui::Selectable(profile.name.c_str(), current)) {
            actions.switch_to_profile = profile.name;
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 110.0f);
        ImGui::TextDisabled("%llu bytes", static_cast<unsigned long long>(profile.size_bytes));
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::Button("Save")) actions.save_requested = true;
    ImGui::SameLine();
    if (ImGui::Button("Reload from disk")) actions.reload_requested = true;
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) refresh_profiles(profiles);

    ImGui::Separator();
    text_field("New profile", new_profile_name_, 64);
    ImGui::SameLine();
    if (ImGui::Button("Create")) {
        const std::string name = ProfileStore::sanitise_profile_name(new_profile_name_);
        if (name.empty()) {
            set_status("That is not a usable profile name.", true);
        } else {
            std::string error;
            if (profiles.save(name, config, error)) {
                new_profile_name_.clear();
                refresh_profiles(profiles);
                set_status("Created '" + name + "'.", false);
            } else {
                set_status(error, true);
            }
        }
    }

    ImGui::Separator();
    if (checkbox("Pick a profile automatically per game", config.general.auto_profile_by_executable,
                 "Matches the game's executable name against the profile mappings; falls back "
                 "to 'default'.")) {
        actions.config_changed = true;
    }

    if (!status_message_.empty()) {
        ImGui::Separator();
        if (status_is_error_) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", status_message_.c_str());
        } else {
            ImGui::TextDisabled("%s", status_message_.c_str());
        }
    }
}

void SettingsUi::draw_diagnostics_tab(const Config& config, const LinkDiagnostics& link,
                                      const OverlayFrame& frame, const FrameStats& stats,
                                      const ConfigDiagnostics& diagnostics,
                                      SettingsActions& actions) {
    ImGui::TextUnformatted("Link");
    ImGui::Separator();
    ImGui::Text("State      %s", to_string(link.state));
    ImGui::Text("Detail     %s", link.detail.c_str());
    ImGui::Text("Endpoint   %s", link.endpoint.c_str());
    if (!link.obs_version.empty()) {
        ImGui::Text("OBS        %s (%s)", link.obs_version.c_str(), link.platform.c_str());
        ImGui::Text("Script     %s", link.script_version.c_str());
    }
    ImGui::Text("Messages   %llu received, %llu malformed",
                static_cast<unsigned long long>(link.messages_received),
                static_cast<unsigned long long>(link.malformed_messages));
    ImGui::Text("Events     %llu received, %llu dropped",
                static_cast<unsigned long long>(link.events_received),
                static_cast<unsigned long long>(link.events_dropped));
    if (link.round_trip_ms >= 0) {
        ImGui::Text("Round trip %lld ms", static_cast<long long>(link.round_trip_ms));
    }
    if (frame.stale) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Nothing has arrived recently; the state below may be out of date.");
    }
    if (ImGui::Button("Reconnect")) actions.reconnect_requested = true;

    ImGui::Spacing();
    ImGui::TextUnformatted("OBS");
    ImGui::Separator();
    if (!frame.state.obs_running) {
        ImGui::TextDisabled("Not connected.");
    } else {
        ImGui::Text("Recording  %s", to_string(frame.state.recording.state));
        ImGui::Text("Replay     %s", to_string(frame.state.replay.state));
        ImGui::Text("Stream     %s %s", to_string(frame.state.stream.state),
                    frame.state.stream.service.c_str());
        ImGui::Text("Camera     %s", to_string(frame.state.virtual_cam.state));
        ImGui::Text("Scene      %s", frame.state.current_scene.c_str());
        if (config.logging.include_paths && !frame.state.recording.path.empty()) {
            ImGui::TextDisabled("%s", frame.state.recording.path.c_str());
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Rendering");
    ImGui::Separator();
    ImGui::Text("Frames     %llu", static_cast<unsigned long long>(stats.frames));
    ImGui::Text("Draw       %.2f ms, %zu commands", static_cast<double>(stats.last_draw_ms),
                stats.draw_commands);
    ImGui::Text("Toasts     %zu on screen", stats.visible_toasts);
    ImGui::Text("Text width %s", text_metrics_source_name(text_metrics_source()));
    if (text_metrics_source() == TextMetricsSource::Estimated) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Widths are estimated, so edges will be a few pixels out.");
    }
    if (fonts_ != nullptr) ImGui::Text("Font       %s", fonts_->status().c_str());
    ImGui::Text("Build      %s (%s)", OBSN_VERSION, OBSN_BUILD_ID);

    if (!diagnostics.issues.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Configuration repairs");
        ImGui::Separator();
        for (const ConfigIssue& issue : diagnostics.issues) {
            const ImVec4 color = issue.severity == ConfigIssue::Severity::Error
                                     ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                     : ImVec4(0.75f, 0.75f, 0.78f, 1.0f);
            ImGui::TextColored(color, "%s  %s",
                               issue.path.empty() ? "(document)" : issue.path.c_str(),
                               issue.message.c_str());
        }
    }
}

SettingsActions SettingsUi::draw(Config& config, const LinkDiagnostics& link,
                                 const OverlayFrame& frame, ProfileStore& profiles,
                                 const FrameStats& stats, const ConfigDiagnostics& diagnostics) {
    SettingsActions actions;

    if (checkbox("Enabled", config.general.enabled)) actions.config_changed = true;
    ImGui::SameLine();
    if (checkbox("Show with OBS closed", config.general.show_when_obs_closed)) {
        actions.config_changed = true;
    }
    ImGui::SameLine();
    if (checkbox("Every setting", config.general.advanced_settings)) actions.config_changed = true;

    // The preview is driven by which tab is open rather than by a toggle: a preview you have to
    // remember to turn off is a preview that gets left on.
    bool preview_now = false;

    if (ImGui::BeginTabBar("obsn_tabs")) {
        if (ImGui::BeginTabItem("Notifications")) {
            draw_notifications_tab(config, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Appearance")) {
            draw_appearance_tab(config, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Status")) {
            draw_status_tab(config, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Preview")) {
            preview_now = true;
            ImGui::TextWrapped(
                "Sample state and one toast of every enabled category, through the real "
                "formatter and the real lifecycle. Nothing here is sent over the pipe, and "
                "nothing is recorded.");
            if (ImGui::Button("Show them again")) actions.seed_preview = true;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Profiles")) {
            draw_profiles_tab(config, profiles, actions);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Diagnostics")) {
            draw_diagnostics_tab(config, link, frame, stats, diagnostics, actions);
            ImGui::EndTabItem();
        }
        if (config.general.advanced_settings && ImGui::BeginTabItem("Link")) {
            bool changed = false;
            changed |= text_field("Pipe name", config.integration.pipe_name);
            help_marker("Must match the name set in the OBS script. Empty uses the default, "
                        "which includes your user SID so two accounts on one machine cannot "
                        "collide.");
            changed |= slider_int("Reconnect delay (ms)",
                                  config.integration.reconnect_initial_ms, 50, 5000);
            changed |= slider_int("Maximum reconnect delay (ms)",
                                  config.integration.reconnect_max_ms, 250, 60000);
            changed |= slider_int("Treat as stale after (ms)",
                                  config.integration.stale_after_ms, 1000, 60000);
            changed |= checkbox("Connect automatically", config.integration.auto_connect);
            ImGui::Separator();
            changed |= text_field("Log level", config.logging.level);
            changed |= checkbox("Write a log file", config.logging.to_file);
            changed |= checkbox("Include file paths in the log", config.logging.include_paths,
                                "Off by default: your recording paths are not written to disk "
                                "by this add-on unless you ask for them.");
            if (changed) actions.config_changed = true;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    preview_active_ = preview_now;
    return actions;
}

}  // namespace obsn::overlay
