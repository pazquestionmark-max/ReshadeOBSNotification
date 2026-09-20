// SPDX-License-Identifier: MIT
#include "obsn/profile_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "obsn/log.hpp"

namespace obsn {
namespace {

constexpr char kComponent[] = "config";
constexpr char kMappingFile[] = "game-profiles.json";
constexpr char kExtension[] = ".json";

std::string lower(std::string_view in) {
    std::string out(in);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    return out;
}

/// Reserved DOS device names. Creating "con.json" on Windows does not do what it looks like.
bool is_reserved_name(const std::string& lowered) {
    static const char* kReserved[] = {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3",
                                      "com4", "com5", "com6", "com7", "com8", "com9", "lpt1",
                                      "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8",
                                      "lpt9"};
    for (const char* reserved : kReserved) {
        if (lowered == reserved) return true;
    }
    return false;
}

std::string read_file(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    ok = true;
    return buffer.str();
}

}  // namespace

bool ProfileStore::valid_profile_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    // No leading or trailing dot or space: those are confusing as file names, and rejecting
    // them here keeps valid_profile_name and sanitise_profile_name in agreement.
    if (name.front() == '.' || name.front() == ' ') return false;
    if (name.back() == '.' || name.back() == ' ') return false;
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20) return false;
        // Rejects separators and traversal outright rather than trying to normalise them.
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            return false;
        }
    }
    return !is_reserved_name(lower(name));
}

std::string ProfileStore::sanitise_profile_name(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20) continue;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            out.push_back('_');
            continue;
        }
        out.push_back(c);
    }
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.size() > 64) out = json::truncate_utf8(out, 64);
    if (out.empty() || is_reserved_name(lower(out))) return {};
    return out;
}

std::string ProfileStore::default_root() {
#if defined(_WIN32)
    char buffer[MAX_PATH];
    std::size_t length = 0;
    if (getenv_s(&length, buffer, sizeof(buffer), "APPDATA") == 0 && length > 1) {
        return std::string(buffer) + "\\OBSNotifyOverlay";
    }
    return {};
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg != '\0') return std::string(xdg) + "/obsn";
    }
    if (const char* home = std::getenv("HOME")) {
        if (*home != '\0') return std::string(home) + "/.config/obsn";
    }
    return {};
#endif
}

ProfileStore::ProfileStore(std::string root) : root_(std::move(root)) {
    if (root_.empty()) root_ = default_root();
}

bool ProfileStore::ensure_root(std::string& error) const {
    if (root_.empty()) {
        error = "no configuration directory could be determined";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(root_) / "profiles", ec);
    if (ec) {
        error = "could not create " + root_ + ": " + ec.message();
        return false;
    }
    return true;
}

std::string ProfileStore::profile_path(std::string_view name) const {
    const std::string safe = valid_profile_name(name) ? std::string(name) : std::string("default");
    return (std::filesystem::path(root_) / "profiles" / (safe + kExtension)).string();
}

std::string ProfileStore::mapping_path() const {
    return (std::filesystem::path(root_) / kMappingFile).string();
}

std::string ProfileStore::active_profile() const {
    bool ok = false;
    const json::Value doc = read_json(mapping_path(), ok);
    if (!ok) return "default";
    const std::string active = doc.get_string("active", "default");
    return valid_profile_name(active) ? active : "default";
}

json::Value ProfileStore::read_json(const std::string& path, bool& ok) const {
    ok = false;
    bool read_ok = false;
    const std::string text = read_file(path, read_ok);
    if (!read_ok) return json::Value(json::Object{});
    json::Limits limits;
    limits.max_total_bytes = 4u * 1024u * 1024u;
    limits.max_object_members = 4096;
    const json::ParseResult parsed = json::parse(text, limits);
    if (!parsed.ok) return json::Value(json::Object{});
    ok = true;
    return parsed.value;
}

Config ProfileStore::load(std::string_view name, ConfigDiagnostics& diag) const {
    const std::string path = profile_path(name);
    bool ok = false;
    const std::string text = read_file(path, ok);
    if (!ok) {
        // A first run has no file. That is not an error, and must not look like one.
        diag.from_defaults = true;
        diag.add(ConfigIssue::Severity::Info, "", "no saved configuration; using defaults");
        Config config = Config::defaults();
        config.general.profile_name = valid_profile_name(name) ? std::string(name) : "default";
        return config;
    }
    Config config = Config::parse(text, diag);
    config.general.profile_name = valid_profile_name(name) ? std::string(name) : "default";
    if (diag.from_defaults) {
        OBSN_WARN(kComponent, "configuration at " + path + " could not be read; using defaults");
    }
    return config;
}

bool ProfileStore::save(std::string_view name, const Config& config, std::string& error) const {
    if (!valid_profile_name(name)) {
        error = "'" + std::string(name) + "' is not a valid profile name";
        return false;
    }
    if (!ensure_root(error)) return false;

    const std::string path = profile_path(name);
    // Write-then-rename: a crash or a full disk leaves the previous configuration intact rather
    // than a truncated file the next launch would reject.
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "could not open " + temporary + " for writing";
            return false;
        }
        const std::string text = config.serialise();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            error = "could not write " + temporary;
            out.close();
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        // Windows rename fails onto an existing file; remove and retry before giving up.
        std::filesystem::remove(path, ec);
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            error = "could not replace " + path + ": " + ec.message();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    OBSN_INFO(kComponent, "saved profile '" + std::string(name) + "'");
    return true;
}

std::vector<ProfileInfo> ProfileStore::list() const {
    std::vector<ProfileInfo> out;
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(root_) / "profiles";
    if (!std::filesystem::exists(dir, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != kExtension) continue;
        ProfileInfo info;
        info.name = entry.path().stem().string();
        if (!valid_profile_name(info.name)) continue;
        info.path = entry.path().string();
        info.size_bytes = static_cast<std::uint64_t>(entry.file_size(ec));
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(),
              [](const ProfileInfo& a, const ProfileInfo& b) { return a.name < b.name; });
    return out;
}

bool ProfileStore::exists(std::string_view name) const {
    if (!valid_profile_name(name)) return false;
    std::error_code ec;
    return std::filesystem::exists(profile_path(name), ec);
}

bool ProfileStore::remove(std::string_view name, std::string& error) const {
    if (!valid_profile_name(name)) {
        error = "invalid profile name";
        return false;
    }
    if (name == "default") {
        // Deleting the fallback would leave automatic profile selection with nowhere to land.
        error = "the default profile cannot be deleted";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::remove(profile_path(name), ec) || ec) {
        error = ec ? ec.message() : "profile not found";
        return false;
    }
    return true;
}

bool ProfileStore::rename(std::string_view from, std::string_view to, std::string& error) const {
    if (!valid_profile_name(from) || !valid_profile_name(to)) {
        error = "invalid profile name";
        return false;
    }
    if (exists(to)) {
        error = "a profile named '" + std::string(to) + "' already exists";
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(profile_path(from), profile_path(to), ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

bool ProfileStore::duplicate(std::string_view from, std::string_view to,
                             std::string& error) const {
    if (!valid_profile_name(from) || !valid_profile_name(to)) {
        error = "invalid profile name";
        return false;
    }
    if (exists(to)) {
        error = "a profile named '" + std::string(to) + "' already exists";
        return false;
    }
    ConfigDiagnostics diag;
    const Config config = load(from, diag);
    return save(to, config, error);
}

bool ProfileStore::export_to(const std::string& path, const Config& config,
                             std::string& error) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "could not open " + path + " for writing";
        return false;
    }
    const std::string text = config.serialise();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        error = "could not write " + path;
        return false;
    }
    return true;
}

Config ProfileStore::import_from(const std::string& path, ConfigDiagnostics& diag,
                                 bool& ok) const {
    bool read_ok = false;
    const std::string text = read_file(path, read_ok);
    if (!read_ok) {
        ok = false;
        diag.add(ConfigIssue::Severity::Error, "", "could not read " + path);
        return Config::defaults();
    }
    Config config = Config::parse(text, diag);
    // An import that produced nothing but defaults is a failed import, and the user should be
    // told rather than left wondering why nothing changed.
    ok = !diag.from_defaults;
    return config;
}

std::string ProfileStore::profile_for_executable(std::string_view executable) const {
    if (executable.empty()) return "default";
    bool ok = false;
    const json::Value doc = read_json(mapping_path(), ok);
    if (!ok) return "default";
    const json::Value* games = doc.find("games");
    if (games == nullptr) return "default";
    const std::string key = lower(executable);
    for (const auto& [name, value] : games->as_object()) {
        if (lower(name) == key && value.is_string()) {
            const std::string profile = value.as_string();
            if (valid_profile_name(profile) && exists(profile)) return profile;
        }
    }
    return "default";
}

bool ProfileStore::map_executable(std::string_view executable, std::string_view profile,
                                  std::string& error) const {
    if (executable.empty()) {
        error = "no executable name";
        return false;
    }
    if (!valid_profile_name(profile)) {
        error = "invalid profile name";
        return false;
    }
    if (!ensure_root(error)) return false;

    bool ok = false;
    json::Value doc = read_json(mapping_path(), ok);
    if (!doc.is_object()) doc = json::Value(json::Object{});
    json::Value games = doc.find("games") != nullptr && doc.find("games")->is_object()
                            ? *doc.find("games")
                            : json::Value(json::Object{});
    games.set(lower(executable), json::Value(std::string(profile)));
    doc.set("games", std::move(games));
    if (doc.find("active") == nullptr) doc.set("active", json::Value("default"));

    std::ofstream out(mapping_path(), std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "could not write " + mapping_path();
        return false;
    }
    const std::string text = doc.dump();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

bool ProfileStore::unmap_executable(std::string_view executable, std::string& error) const {
    bool ok = false;
    json::Value doc = read_json(mapping_path(), ok);
    if (!ok) return true;  // nothing mapped: already in the requested state
    const json::Value* games = doc.find("games");
    if (games == nullptr) return true;

    json::Value replacement{json::Object{}};
    const std::string key = lower(executable);
    for (const auto& [name, value] : games->as_object()) {
        if (lower(name) != key) replacement.set(name, value);
    }
    doc.set("games", std::move(replacement));

    std::ofstream out(mapping_path(), std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "could not write " + mapping_path();
        return false;
    }
    const std::string text = doc.dump();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::vector<std::pair<std::string, std::string>> ProfileStore::executable_mappings() const {
    std::vector<std::pair<std::string, std::string>> out;
    bool ok = false;
    const json::Value doc = read_json(mapping_path(), ok);
    if (!ok) return out;
    const json::Value* games = doc.find("games");
    if (games == nullptr) return out;
    for (const auto& [name, value] : games->as_object()) {
        if (value.is_string()) out.emplace_back(name, value.as_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string current_executable_name() {
#if defined(_WIN32)
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const std::string full(path, n);
    const std::size_t slash = full.find_last_of("\\/");
    return lower(slash == std::string::npos ? full : full.substr(slash + 1));
#else
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return lower(self.filename().string());
#endif
}

}  // namespace obsn
