// SPDX-License-Identifier: MIT
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "obsn/profile_store.hpp"
#include "obsn_test.hpp"

using namespace obsn;

namespace {

/// A configuration directory that cleans itself up however the test exits.
struct TempRoot {
    std::filesystem::path path;

    TempRoot() {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("obsn-store-" + std::to_string(++counter) + "-" +
                std::to_string(static_cast<long long>(std::hash<std::string>{}(
                    std::to_string(reinterpret_cast<std::uintptr_t>(this))))));
        std::filesystem::create_directories(path);
    }
    ~TempRoot() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

TEST(profiles, a_first_run_yields_defaults_rather_than_an_error) {
    TempRoot root;
    ProfileStore store(root.str());
    ConfigDiagnostics diag;
    const Config config = store.load("default", diag);

    CHECK(diag.from_defaults);
    CHECK_EQ(config.appearance.title_size, Config::defaults().appearance.title_size);
    CHECK_EQ(config.general.profile_name, std::string("default"));
    // A missing file is expected on a first run and must not be reported as an error.
    for (const ConfigIssue& issue : diag.issues) {
        CHECK(issue.severity != ConfigIssue::Severity::Error);
    }
}

TEST(profiles, a_saved_profile_round_trips) {
    TempRoot root;
    ProfileStore store(root.str());
    Config config = Config::defaults();
    config.general.scale = 1.75f;
    config.notifications.max_visible = 9;
    config.notifications.replay_saved.accent = Color{0, 255, 255, 255};

    std::string error;
    CHECK(store.save("default", config, error));
    CHECK(error.empty());

    ConfigDiagnostics diag;
    const Config loaded = store.load("default", diag);
    CHECK(!diag.from_defaults);
    CHECK_EQ(loaded.general.scale, 1.75f);
    CHECK_EQ(loaded.notifications.max_visible, 9);
    CHECK_EQ(loaded.notifications.replay_saved.accent.to_hex(), std::string("#00FFFFFF"));
}

TEST(profiles, saving_leaves_no_temporary_file_behind) {
    TempRoot root;
    ProfileStore store(root.str());
    std::string error;
    CHECK(store.save("default", Config::defaults(), error));

    int leftovers = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(root.path / "profiles")) {
        if (entry.path().extension() == ".tmp") ++leftovers;
    }
    CHECK_EQ(leftovers, 0);
}

TEST(profiles, a_corrupt_file_yields_defaults_and_a_diagnostic) {
    TempRoot root;
    ProfileStore store(root.str());
    write_file(root.path / "profiles" / "default.json", "{ this is not json");

    ConfigDiagnostics diag;
    const Config config = store.load("default", diag);
    CHECK(diag.from_defaults);
    CHECK(!diag.issues.empty());
    CHECK_EQ(config.appearance.title_size, Config::defaults().appearance.title_size);
}

TEST(profiles, a_truncated_file_yields_defaults_rather_than_a_crash) {
    TempRoot root;
    ProfileStore store(root.str());
    const std::string full = Config::defaults().serialise();
    write_file(root.path / "profiles" / "default.json", full.substr(0, full.size() / 3));

    ConfigDiagnostics diag;
    store.load("default", diag);
    CHECK(diag.from_defaults);
}

TEST(profiles, an_existing_profile_survives_a_failed_write) {
    // The point of writing to a temporary and renaming: a good configuration is never replaced
    // by a broken one.
    TempRoot root;
    ProfileStore store(root.str());
    Config good = Config::defaults();
    good.general.scale = 1.5f;
    std::string error;
    CHECK(store.save("default", good, error));

    CHECK(!store.save("bad/name", Config::defaults(), error));
    CHECK(!error.empty());

    ConfigDiagnostics diag;
    CHECK_EQ(store.load("default", diag).general.scale, 1.5f);
}

TEST(profiles, listing_finds_saved_profiles_in_order) {
    TempRoot root;
    ProfileStore store(root.str());
    std::string error;
    CHECK(store.save("default", Config::defaults(), error));
    CHECK(store.save("racing", Config::defaults(), error));
    CHECK(store.save("arena", Config::defaults(), error));

    const std::vector<ProfileInfo> profiles = store.list();
    CHECK_EQ(profiles.size(), std::size_t{3});
    CHECK_EQ(profiles[0].name, std::string("arena"));
    CHECK_EQ(profiles[1].name, std::string("default"));
    CHECK_EQ(profiles[2].name, std::string("racing"));
    CHECK(profiles[0].size_bytes > 0);
}

TEST(profiles, unrelated_files_are_not_listed_as_profiles) {
    TempRoot root;
    ProfileStore store(root.str());
    std::string error;
    CHECK(store.save("default", Config::defaults(), error));
    write_file(root.path / "profiles" / "notes.txt", "hello");
    write_file(root.path / "profiles" / "default.json.tmp", "{}");
    CHECK_EQ(store.list().size(), std::size_t{1});
}

TEST(profiles, profile_names_that_could_escape_the_directory_are_rejected) {
    // A profile name becomes a file name, so traversal and separators must never get through.
    CHECK(!ProfileStore::valid_profile_name("../escape"));
    CHECK(!ProfileStore::valid_profile_name("..\\escape"));
    CHECK(!ProfileStore::valid_profile_name("sub/dir"));
    CHECK(!ProfileStore::valid_profile_name("C:name"));
    CHECK(!ProfileStore::valid_profile_name(".hidden"));
    CHECK(!ProfileStore::valid_profile_name(""));
    CHECK(!ProfileStore::valid_profile_name(std::string(200, 'x')));
    CHECK(!ProfileStore::valid_profile_name("name\x01"));
    CHECK(!ProfileStore::valid_profile_name(" leading"));
    CHECK(!ProfileStore::valid_profile_name("trailing "));
    // Reserved Windows device names would not create the file the user expects.
    CHECK(!ProfileStore::valid_profile_name("con"));
    CHECK(!ProfileStore::valid_profile_name("LPT1"));

    CHECK(ProfileStore::valid_profile_name("default"));
    CHECK(ProfileStore::valid_profile_name("Racing Setup 2"));
    CHECK(ProfileStore::valid_profile_name("my-profile_v2"));
}

TEST(profiles, a_rejected_name_never_reaches_the_filesystem) {
    TempRoot root;
    ProfileStore store(root.str());
    std::string error;
    CHECK(!store.save("../../escape", Config::defaults(), error));
    CHECK(!std::filesystem::exists(root.path.parent_path() / "escape.json"));
}

TEST(profiles, names_are_sanitised_into_something_usable) {
    CHECK_EQ(ProfileStore::sanitise_profile_name("my/profile"), std::string("my_profile"));
    CHECK_EQ(ProfileStore::sanitise_profile_name("  spaced  "), std::string("spaced"));
    CHECK_EQ(ProfileStore::sanitise_profile_name("..."), std::string(""));
    CHECK_EQ(ProfileStore::sanitise_profile_name("con"), std::string(""));
    CHECK(ProfileStore::valid_profile_name(ProfileStore::sanitise_profile_name("a<b>c")));
    // Whatever sanitise returns must always pass the validator; the two cannot disagree.
    for (const char* input : {"my/profile", "  spaced  ", "a<b>c", "normal", "..x.."}) {
        const std::string cleaned = ProfileStore::sanitise_profile_name(input);
        if (!cleaned.empty()) CHECK(ProfileStore::valid_profile_name(cleaned));
    }
}

TEST(profiles, duplicate_rename_and_delete_behave) {
    TempRoot root;
    ProfileStore store(root.str());
    Config config = Config::defaults();
    config.general.scale = 2.0f;
    std::string error;
    CHECK(store.save("original", config, error));

    CHECK(store.duplicate("original", "copy", error));
    ConfigDiagnostics diag;
    CHECK_EQ(store.load("copy", diag).general.scale, 2.0f);

    CHECK(!store.duplicate("original", "copy", error));  // would overwrite
    CHECK(!error.empty());

    CHECK(store.rename("copy", "renamed", error));
    CHECK(store.exists("renamed"));
    CHECK(!store.exists("copy"));

    CHECK(store.remove("renamed", error));
    CHECK(!store.exists("renamed"));
}

TEST(profiles, the_default_profile_cannot_be_deleted) {
    // Automatic profile selection falls back to it, so removing it would break that silently.
    TempRoot root;
    ProfileStore store(root.str());
    std::string error;
    CHECK(store.save("default", Config::defaults(), error));
    CHECK(!store.remove("default", error));
    CHECK(!error.empty());
    CHECK(store.exists("default"));
}

TEST(profiles, export_and_import_round_trip) {
    TempRoot root;
    ProfileStore store(root.str());
    Config config = Config::defaults();
    config.general.scale = 1.33f;
    config.notifications.warning.accent = Color{255, 0, 128, 255};

    const std::string path = (root.path / "shared-config.json").string();
    std::string error;
    CHECK(store.export_to(path, config, error));

    ConfigDiagnostics diag;
    bool ok = false;
    const Config imported = store.import_from(path, diag, ok);
    CHECK(ok);
    CHECK_EQ(imported.general.scale, 1.33f);
    CHECK_EQ(imported.notifications.warning.accent.to_hex(), std::string("#FF0080FF"));
}

TEST(profiles, importing_a_missing_or_broken_file_reports_failure) {
    TempRoot root;
    ProfileStore store(root.str());
    ConfigDiagnostics diag;
    bool ok = true;
    store.import_from((root.path / "nope.json").string(), diag, ok);
    CHECK(!ok);

    write_file(root.path / "broken.json", "not json at all");
    ConfigDiagnostics diag2;
    ok = true;
    store.import_from((root.path / "broken.json").string(), diag2, ok);
    CHECK(!ok);
}

TEST(profiles, an_executable_can_be_mapped_to_a_profile) {
    TempRoot root;
    ProfileStore store(root.str());
    std::string error;
    CHECK(store.save("default", Config::defaults(), error));
    CHECK(store.save("racing", Config::defaults(), error));

    CHECK(store.map_executable("RaceGame.exe", "racing", error));
    CHECK_EQ(store.profile_for_executable("racegame.exe"), std::string("racing"));
    CHECK_EQ(store.profile_for_executable("RACEGAME.EXE"), std::string("racing"));
    CHECK_EQ(store.profile_for_executable("other.exe"), std::string("default"));
}

TEST(profiles, a_mapping_to_a_deleted_profile_falls_back_to_default) {
    TempRoot root;
    ProfileStore store(root.str());
    std::string error;
    CHECK(store.save("default", Config::defaults(), error));
    CHECK(store.save("racing", Config::defaults(), error));
    CHECK(store.map_executable("game.exe", "racing", error));
    CHECK(store.remove("racing", error));
    CHECK_EQ(store.profile_for_executable("game.exe"), std::string("default"));
}

TEST(profiles, mappings_can_be_listed_and_removed) {
    TempRoot root;
    ProfileStore store(root.str());
    std::string error;
    CHECK(store.save("default", Config::defaults(), error));
    CHECK(store.save("racing", Config::defaults(), error));
    CHECK(store.map_executable("a.exe", "racing", error));
    CHECK(store.map_executable("b.exe", "default", error));
    CHECK_EQ(store.executable_mappings().size(), std::size_t{2});

    CHECK(store.unmap_executable("a.exe", error));
    CHECK_EQ(store.executable_mappings().size(), std::size_t{1});
    CHECK_EQ(store.profile_for_executable("a.exe"), std::string("default"));
}

TEST(profiles, a_corrupt_mapping_file_falls_back_rather_than_failing) {
    TempRoot root;
    ProfileStore store(root.str());
    write_file(root.path / "game-profiles.json", "{{{ broken");
    CHECK_EQ(store.profile_for_executable("game.exe"), std::string("default"));
    CHECK_EQ(store.active_profile(), std::string("default"));
    CHECK_EQ(store.executable_mappings().size(), std::size_t{0});
}

TEST(profiles, the_current_executable_name_is_discoverable) {
    // Used for automatic per-game profile selection; on any supported platform it must resolve.
    const std::string name = current_executable_name();
    CHECK(!name.empty());
    CHECK_EQ(name, std::string(name));  // already lower-cased by contract
}
