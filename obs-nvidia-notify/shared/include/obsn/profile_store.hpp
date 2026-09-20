// SPDX-License-Identifier: MIT
// Configuration persistence: named profiles, import/export and per-executable selection.
//
// Lives in shared/ rather than in the add-on because it is ordinary file handling with real
// failure modes (a half-written file, a missing directory, a config from a newer build), and
// those are worth testing directly rather than only in a game.
#ifndef OBSN_PROFILE_STORE_HPP
#define OBSN_PROFILE_STORE_HPP

#include <string>
#include <vector>

#include "obsn/config.hpp"

namespace obsn {

struct ProfileInfo {
    std::string name;
    std::string path;
    std::uint64_t size_bytes = 0;
};

class ProfileStore {
public:
    /// `%APPDATA%\OBSNotifyOverlay` on Windows; `$XDG_CONFIG_HOME/obsn` (or
    /// `~/.config/obsn`) elsewhere. Empty if neither can be determined.
    static std::string default_root();

    explicit ProfileStore(std::string root);

    const std::string& root() const noexcept { return root_; }
    std::string profile_path(std::string_view name) const;
    std::string active_profile() const;

    /// Creates the configuration directory if it is missing. Returns false and fills `error`
    /// when the location is unusable, which the Diagnostics tab surfaces rather than the add-on
    /// silently running without persistence.
    bool ensure_root(std::string& error) const;

    /// Loads a profile. A missing file yields defaults with `from_defaults` set — not an error,
    /// because a first run has no file. A corrupt file yields defaults plus a diagnostic.
    Config load(std::string_view name, ConfigDiagnostics& diag) const;

    /// Writes atomically: a temporary file in the same directory, then a rename. A crash or a
    /// full disk mid-write therefore leaves the previous configuration intact rather than a
    /// truncated one.
    bool save(std::string_view name, const Config& config, std::string& error) const;

    std::vector<ProfileInfo> list() const;
    bool exists(std::string_view name) const;
    bool remove(std::string_view name, std::string& error) const;
    bool rename(std::string_view from, std::string_view to, std::string& error) const;
    bool duplicate(std::string_view from, std::string_view to, std::string& error) const;

    /// Export and import use absolute paths so a user can share a configuration as a file.
    bool export_to(const std::string& path, const Config& config, std::string& error) const;
    Config import_from(const std::string& path, ConfigDiagnostics& diag, bool& ok) const;

    // --- per-executable profile selection ---
    /// Returns the profile mapped to `executable` (case-insensitively), or "default".
    std::string profile_for_executable(std::string_view executable) const;
    bool map_executable(std::string_view executable, std::string_view profile,
                        std::string& error) const;
    bool unmap_executable(std::string_view executable, std::string& error) const;
    std::vector<std::pair<std::string, std::string>> executable_mappings() const;

    /// True when `name` is safe to use as a file name: no separators, no traversal, no reserved
    /// Windows device names. Enforced on every path that builds a file name from user input.
    static bool valid_profile_name(std::string_view name);
    /// Sanitises an arbitrary string into a valid profile name, or "" if nothing usable remains.
    static std::string sanitise_profile_name(std::string_view name);

private:
    std::string mapping_path() const;
    json::Value read_json(const std::string& path, bool& ok) const;

    std::string root_;
};

/// Base name of the current process's executable, lower-cased (e.g. "game.exe"). Empty when it
/// cannot be determined; callers then fall back to the default profile.
std::string current_executable_name();

}  // namespace obsn

#endif  // OBSN_PROFILE_STORE_HPP
