// SPDX-License-Identifier: MIT
// ReShade add-on entry point.
//
// Two registrations, each chosen for when ReShade invokes it:
//
//   * addon_event::reshade_overlay -- called every frame, between ImGui::NewFrame and
//     ImGui::EndFrame. ReShade's early-out explicitly keeps building an ImGui frame when an
//     add-on has subscribed to this event, which is what makes an always-on overlay possible.
//     We draw into the background draw list, so we add no draw call batch of our own and take
//     no input.
//
//   * register_overlay("...") -- called only while ReShade's menu is open. That is exactly when
//     the user is configuring and when ReShade is already blocking game input, so it is the
//     right home for the settings window.
//
// Nothing here blocks: the IPC connection lives on OverlayClient's own thread and the render
// callback only reads a triple-buffered frame.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
// Order matters: reshade.hpp defines the ImGui:: and ImDrawList:: members that imgui.h only
// declares, routing them through ReShade's function table.
#include <reshade.hpp>

#include "font_engine.hpp"
#include "obsn/log.hpp"
#include "obsn/overlay_client.hpp"
#include "obsn/profile_store.hpp"
#include "renderer.hpp"
#include "settings_ui.hpp"

extern "C" __declspec(dllexport) const char* NAME = "OBS Notifications";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Shows OBS capture events -- recording, replay buffer, streaming -- as notifications inside "
    "the game, fed by the OBS Notify companion script over a local named pipe. Opens no network "
    "connection.";

namespace {

constexpr char kComponent[] = "addon";

/// Everything the add-on owns, so lifetime is one object rather than a scatter of globals.
struct AddonState {
    obsn::Config config = obsn::Config::defaults();
    obsn::ConfigDiagnostics config_diagnostics;
    std::unique_ptr<obsn::ProfileStore> profiles;
    std::unique_ptr<obsn::OverlayClient> client;
    obsn::overlay::Renderer renderer;
    obsn::overlay::SettingsUi settings;
    /// The overlay's own typeface. Independent of ReShade's font, which stays as the user set it
    /// for ReShade's own UI.
    obsn::overlay::FontEngine fonts;
    std::string applied_font_file;
    int applied_font_face = -1;
    int applied_weight = -1;

    /// Guards `config` only. The render callback reads it and the settings callback writes it;
    /// both run on the same thread inside ReShade's ImGui frame, so this is uncontended in
    /// practice and exists to make the ordering explicit rather than assumed.
    std::mutex config_mutex;

    bool preview_was_active = false;
    bool link_was_up = false;
    std::atomic<bool> started{false};
    std::string profile_name = "default";
};

AddonState* g_state = nullptr;
HMODULE g_module = nullptr;

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/// The folder the add-on DLL was loaded from, which is where its shipped `fonts` folder sits.
std::string module_directory() {
    if (g_module == nullptr) return {};
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(g_module, path, static_cast<DWORD>(sizeof(path)));
    if (n == 0 || n >= sizeof(path)) return {};
    std::string s(path, n);
    const std::size_t cut = s.find_last_of("\\/");
    return cut == std::string::npos ? std::string{} : s.substr(0, cut);
}

/// Loads the configured typeface if it is not already loaded. Cheap to call every frame.
void apply_font(AddonState& state) {
    const obsn::AppearanceConfig& a = state.config.appearance;
    // The title weight drives the atlas; the detail line is drawn from the same face, so a
    // single bake serves both and the detail weight is applied as a lighter colour rather than
    // a second atlas. Two atlases would double the texture memory for a difference of one
    // stroke width on a 13px line.
    if (a.title_weight != state.applied_weight) {
        state.applied_weight = a.title_weight;
        state.fonts.set_weight(a.title_weight);
    }
    if (a.font_file == state.applied_font_file && a.font_face_index == state.applied_font_face) {
        return;
    }
    state.applied_font_file = a.font_file;
    state.applied_font_face = a.font_face_index;
    if (!state.fonts.select(a.font_file, a.font_face_index)) {
        OBSN_WARN(kComponent, "font: " + state.fonts.error());
    }
}

void apply_logging(const obsn::Config& config, const std::string& root) {
    std::string path = config.logging.file_name;
    if (path.empty() && !root.empty()) path = root + "/obsn-overlay.log";
    obsn::Logger::instance().configure(obsn::parse_log_level(config.logging.level),
                                       config.logging.to_file, path, config.logging.max_file_kb);
    // The logger's "message content" switch is this project's "include paths" switch: recording
    // paths are the only user content that crosses this pipe.
    obsn::Logger::instance().set_include_message_content(config.logging.include_paths);
}

void start_client(AddonState& state) {
    obsn::OverlayClientConfig client_config;
    client_config.endpoint = state.config.integration.pipe_name;
    client_config.reconnect_initial_ms = state.config.integration.reconnect_initial_ms;
    client_config.reconnect_max_ms = state.config.integration.reconnect_max_ms;
    client_config.stale_after_ms = state.config.integration.stale_after_ms;
    client_config.ping_interval_ms = state.config.integration.ping_interval_ms;
    client_config.hello.client = "reshade-addon";
    client_config.hello.client_version = OBSN_VERSION;
    client_config.hello.process = obsn::current_executable_name();
    client_config.hello.pid = static_cast<std::int64_t>(GetCurrentProcessId());

    state.client = std::make_unique<obsn::OverlayClient>(std::move(client_config));
    if (state.config.integration.auto_connect) state.client->start();
}

void load_configuration(AddonState& state) {
    const std::string root = obsn::ProfileStore::default_root();
    state.profiles = std::make_unique<obsn::ProfileStore>(root);
    std::string error;
    if (!state.profiles->ensure_root(error)) {
        OBSN_WARN(kComponent, "configuration directory unavailable: " + error);
    }

    // Per-game profile selection: exact executable match, then the default. One lookup at load,
    // not per frame.
    std::string profile = "default";
    obsn::ConfigDiagnostics probe;
    const obsn::Config probe_config = state.profiles->load("default", probe);
    if (probe_config.general.auto_profile_by_executable) {
        const std::string executable = obsn::current_executable_name();
        if (!executable.empty()) profile = state.profiles->profile_for_executable(executable);
    }

    state.config_diagnostics = obsn::ConfigDiagnostics{};
    state.config = state.profiles->load(profile, state.config_diagnostics);
    state.config.general.profile_name = profile;
    state.profile_name = profile;
    apply_logging(state.config, state.profiles->root());

    OBSN_INFO(kComponent, "loaded profile '" + profile + "'");
    for (const obsn::ConfigIssue& issue : state.config_diagnostics.issues) {
        if (issue.severity == obsn::ConfigIssue::Severity::Info) continue;
        OBSN_WARN(kComponent, "configuration: " + issue.path + " " + issue.message);
    }
}

/// Called every frame by ReShade, inside its ImGui frame.
void on_reshade_overlay(reshade::api::effect_runtime* runtime) {
    AddonState* state = g_state;
    if (state == nullptr || !state->started.load(std::memory_order_acquire)) return;
    if (runtime == nullptr) return;

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    if (draw_list == nullptr) return;

    // The viewport comes from ReShade's own API rather than ImGuiIO::DisplaySize.
    //
    // Reading a field off ImGuiIO means trusting that this add-on's imgui.h lays the struct out
    // exactly as ReShade's ImGui build does, and ReShade's function table exists precisely
    // because that cannot be assumed. get_screenshot_width_and_height is a virtual call across a
    // versioned interface, so there is no layout to guess at.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);
    obsn::Viewport viewport;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    if (viewport.width < 1.0f || viewport.height < 1.0f) return;

    const std::int64_t at = now_ms();
    const obsn::OverlayFrame& frame = state->client->latest();

    std::lock_guard<std::mutex> lock(state->config_mutex);

    // Font upkeep happens here, before anything is drawn: begin_frame bakes at most one atlas,
    // so switching typeface or size costs a single frame rather than a stall mid-draw-list.
    apply_font(*state);
    state->fonts.begin_frame(runtime->get_device());

    // The link coming up is an event in its own right, raised here rather than by the client so
    // the suppression window is seeded on the same thread that will read it.
    const bool link_up = frame.state.obs_running;
    if (link_up && !state->link_was_up) {
        obsn::ObsEvent connected;
        connected.kind = obsn::EventKind::ScriptConnected;
        connected.ts = at;
        state->renderer.note_connected(at);
        state->renderer.submit_events({connected}, state->config, frame.state, at);
    }
    state->link_was_up = link_up;

    const std::vector<obsn::ObsEvent> events = state->client->drain_events();
    state->renderer.submit_events(events, state->config, frame.state, at);

    const bool preview = state->settings.preview_active();
    obsn::ObsState preview_state;
    if (preview) {
        preview_state = obsn::overlay::preview_state(at);
        // Seed on entry, and again once the samples have aged out, so the preview keeps showing
        // every category instead of emptying after a few seconds.
        if (!state->preview_was_active || state->renderer.notifications_empty()) {
            state->renderer.seed_preview_notifications(state->config, at);
        }
    } else if (state->preview_was_active) {
        // Leaving the preview must not leave sample toasts on screen.
        state->renderer.clear_notifications();
        state->renderer.note_connected(at);
    }
    state->preview_was_active = preview;

    state->renderer.draw(draw_list, state->config, frame, viewport, at,
                         preview ? &preview_state : nullptr);
}

/// Called by ReShade only while its menu is open.
void on_settings_overlay(reshade::api::effect_runtime* runtime) {
    (void)runtime;
    AddonState* state = g_state;
    if (state == nullptr || !state->started.load(std::memory_order_acquire)) return;

    const obsn::OverlayFrame& frame = state->client->latest();
    const obsn::LinkDiagnostics diagnostics = state->client->diagnostics();

    std::lock_guard<std::mutex> lock(state->config_mutex);
    const obsn::overlay::SettingsActions actions =
        state->settings.draw(state->config, diagnostics, frame, *state->profiles,
                             state->renderer.stats(), state->config_diagnostics);

    const std::int64_t at = now_ms();

    if (actions.config_changed) {
        state->config.clamp(state->config_diagnostics);
        apply_logging(state->config, state->profiles->root());
        state->client->set_stale_after_ms(state->config.integration.stale_after_ms);
    }
    if (actions.reconnect_requested) state->client->request_reconnect();
    if (actions.test_notification != obsn::EventKind::Unknown) {
        state->renderer.test_notification(actions.test_notification, state->config, at);
    }
    if (actions.seed_preview) state->renderer.seed_preview_notifications(state->config, at);

    if (actions.save_requested) {
        std::string error;
        if (state->profiles->save(state->profile_name, state->config, error)) {
            state->settings.set_status("Saved.", false);
            state->settings.refresh_profiles(*state->profiles);
        } else {
            state->settings.set_status(error, true);
        }
    }
    if (actions.reload_requested) {
        state->config_diagnostics = obsn::ConfigDiagnostics{};
        state->config = state->profiles->load(state->profile_name, state->config_diagnostics);
        state->settings.set_status("Reloaded from disk.", false);
    }
    if (!actions.switch_to_profile.empty()) {
        state->config_diagnostics = obsn::ConfigDiagnostics{};
        state->config =
            state->profiles->load(actions.switch_to_profile, state->config_diagnostics);
        state->config.general.profile_name = actions.switch_to_profile;
        state->profile_name = actions.switch_to_profile;
        state->renderer.clear_notifications();
        apply_logging(state->config, state->profiles->root());
        state->settings.set_status("Loaded profile '" + actions.switch_to_profile + "'.", false);
    }
}

bool initialise(HMODULE module) {
    g_module = module;
    // register_addon also resolves ReShade's ImGui function table, and fails if this add-on was
    // built against an ImGui version ReShade does not export. Failing here rather than drawing
    // through a null table is what turns a version mismatch into "the add-on did not load"
    // instead of a crash inside the game.
    if (!reshade::register_addon(module)) return false;

    g_state = new AddonState();
    load_configuration(*g_state);
    // The user's own folder first, then the one shipped beside the add-on, so a font dropped in
    // the config directory wins over one of the same name that came with the release. The
    // config folder is created here so there is somewhere obvious to drop a .ttf even before
    // one has been added -- the settings window prints both paths.
    {
        const std::string user_fonts = g_state->profiles->root() + "/fonts";
        std::error_code ec;
        std::filesystem::create_directories(user_fonts, ec);
        g_state->fonts.set_directories({user_fonts, module_directory() + "/fonts"});
    }
    g_state->renderer.set_font_engine(&g_state->fonts);
    g_state->settings.set_font_engine(&g_state->fonts);
    start_client(*g_state);
    g_state->settings.refresh_profiles(*g_state->profiles);
    g_state->started.store(true, std::memory_order_release);

    reshade::register_event<reshade::addon_event::reshade_overlay>(&on_reshade_overlay);
    reshade::register_overlay("OBS Notifications", &on_settings_overlay);
    OBSN_INFO(kComponent, "add-on initialised");
    return true;
}

void shutdown(HMODULE module) {
    if (g_state != nullptr) {
        g_state->started.store(false, std::memory_order_release);
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(&on_reshade_overlay);
        reshade::unregister_overlay("OBS Notifications", &on_settings_overlay);
        // Stop the IPC thread before the state it references is destroyed.
        if (g_state->client) g_state->client->stop();
        g_state->renderer.set_font_engine(nullptr);
        g_state->settings.set_font_engine(nullptr);
        g_state->fonts.release();
        delete g_state;
        g_state = nullptr;
    }
    reshade::unregister_addon(module);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            // No thread is created here: OverlayClient starts its own, which DllMain is allowed
            // to request because CreateThread does not take the loader lock's critical path for
            // work we do not wait on.
            DisableThreadLibraryCalls(module);
            if (!initialise(module)) return FALSE;
            break;
        case DLL_PROCESS_DETACH:
            shutdown(module);
            break;
        default:
            break;
    }
    return TRUE;
}
