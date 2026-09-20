// SPDX-License-Identifier: MIT
// The in-game settings window, drawn inside ReShade's own menu.
//
// It edits the live Config in place and reports, through SettingsActions, the things it cannot
// do itself: saving, reloading, switching profile and reconnecting all belong to the add-on,
// which owns the ProfileStore and the link. Keeping those out of here is what stops the UI
// needing a pointer to everything.
#ifndef OBSN_SETTINGS_UI_HPP
#define OBSN_SETTINGS_UI_HPP

#include <string>
#include <vector>

#include "font_engine.hpp"
#include "obsn/config.hpp"
#include "obsn/overlay_client.hpp"
#include "obsn/profile_store.hpp"
#include "renderer.hpp"

namespace obsn::overlay {

/// What the window is asking the add-on to do. Every field defaults to "nothing".
struct SettingsActions {
    bool config_changed = false;
    bool save_requested = false;
    bool reload_requested = false;
    bool reconnect_requested = false;
    std::string switch_to_profile;
    /// A category the user pressed "Test" on. Unknown means none.
    EventKind test_notification = EventKind::Unknown;
    bool seed_preview = false;
};

class SettingsUi {
public:
    SettingsActions draw(Config& config, const LinkDiagnostics& link, const OverlayFrame& frame,
                         ProfileStore& profiles, const FrameStats& stats,
                         const ConfigDiagnostics& diagnostics);

    /// True while the Preview tab is open, which is when the add-on feeds the renderer sample
    /// state instead of the live one.
    bool preview_active() const noexcept { return preview_active_; }

    void set_font_engine(FontEngine* fonts) noexcept { fonts_ = fonts; }
    void refresh_profiles(const ProfileStore& profiles);
    void set_status(std::string message, bool is_error);

private:
    void draw_notifications_tab(Config& config, SettingsActions& actions);
    void draw_category(const CategoryEntry& entry, Config& config, SettingsActions& actions);
    void draw_appearance_tab(Config& config, SettingsActions& actions);
    void draw_status_tab(Config& config, SettingsActions& actions);
    void draw_profiles_tab(Config& config, ProfileStore& profiles, SettingsActions& actions);
    void draw_diagnostics_tab(const Config& config, const LinkDiagnostics& link,
                              const OverlayFrame& frame, const FrameStats& stats,
                              const ConfigDiagnostics& diagnostics, SettingsActions& actions);

    FontEngine* fonts_ = nullptr;
    bool preview_active_ = false;
    std::vector<ProfileInfo> profiles_;
    std::string new_profile_name_;
    std::string status_message_;
    bool status_is_error_ = false;
    /// Which category is expanded. Only one at a time: eighteen open panels is not a settings
    /// window, it is a wall.
    int open_category_ = -1;
};

}  // namespace obsn::overlay

#endif  // OBSN_SETTINGS_UI_HPP
