// SPDX-License-Identifier: MIT
// obsn-config: defaults, validation and schema probing for overlay profiles.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <chrono>
#include <thread>

#include "obsn/config.hpp"
#include "obsn/notifications.hpp"
#include "obsn/overlay_client.hpp"

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: obsn-config <command> [arguments]\n"
                 "\n"
                 "  defaults                 write the shipped configuration to stdout\n"
                 "  validate <file>...       load each file and report every repair\n"
                 "  normalise <file>         load a file and write it back out, canonicalised\n"
                 "  schema                   list every setting with its type and default\n"
                 "  categories               list the notification categories\n"
                 "  probe [seconds] [endpoint]\n"
                 "                           attach to a running OBS script and print what\n"
                 "                           arrives, as the overlay would receive it\n");
    return 2;
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

const char* severity_name(obsn::ConfigIssue::Severity s) {
    switch (s) {
        case obsn::ConfigIssue::Severity::Info: return "info";
        case obsn::ConfigIssue::Severity::Warning: return "warning";
        case obsn::ConfigIssue::Severity::Error: return "error";
    }
    return "?";
}

/// Reports every repair the loader had to perform. An `error` is a file that could not be used
/// at all; a `warning` is a value that was ignored; an `info` is a value that was clamped. Only
/// the first fails the command, because a clamped number is a working configuration.
int validate(const std::vector<std::string>& paths) {
    int failures = 0;
    for (const std::string& path : paths) {
        std::string text;
        if (!read_file(path, text)) {
            std::fprintf(stderr, "%s: cannot be read\n", path.c_str());
            ++failures;
            continue;
        }
        obsn::ConfigDiagnostics diag;
        const obsn::Config config = obsn::Config::parse(text, diag);
        (void)config;

        bool fatal = false;
        for (const obsn::ConfigIssue& issue : diag.issues) {
            std::printf("%s: %s: %s %s\n", path.c_str(), severity_name(issue.severity),
                        issue.path.empty() ? "(document)" : issue.path.c_str(),
                        issue.message.c_str());
            if (issue.severity == obsn::ConfigIssue::Severity::Error) fatal = true;
        }
        if (fatal) {
            ++failures;
        } else {
            std::printf("%s: ok%s\n", path.c_str(),
                        diag.issues.empty() ? "" : " (with repairs, listed above)");
        }
    }
    return failures == 0 ? 0 : 1;
}

int schema() {
    // Printed from the serialised defaults rather than from a second, hand-written list: a
    // setting that exists but is missing here would be a setting nobody can discover.
    const obsn::Config defaults = obsn::Config::defaults();
    std::cout << defaults.serialise() << "\n";
    return 0;
}

int list_categories() {
    for (const obsn::CategoryEntry& entry : obsn::categories()) {
        std::printf("%-22s %-24s %s\n", entry.key, entry.label, obsn::to_string(entry.kind));
    }
    return 0;
}


/// Attaches to a running OBS script and prints what arrives.
///
/// This is the same OverlayClient the add-on runs, against the same endpoint, so "the script is
/// not sending anything" and "the overlay is not drawing it" become separable questions without
/// needing a game. It also makes the two implementations testable against each other in CI.
int probe(int seconds, const std::string& endpoint) {
    obsn::OverlayClientConfig config;
    config.endpoint = endpoint;
    config.ping_interval_ms = 2000;
    config.hello.client = "obsn-config";
    config.hello.client_version = OBSN_VERSION;
    config.hello.process = "obsn-config";

    obsn::OverlayClient client(config);
    client.start();

    const obsn::Config styling = obsn::Config::defaults();
    obsn::NotificationQueue notifications;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    obsn::LinkState last_link = obsn::LinkState::Idle;
    int event_count = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        const obsn::OverlayFrame& frame = client.latest();
        if (frame.link != last_link) {
            last_link = frame.link;
            std::printf("link: %s (%s)\n", obsn::to_string(frame.link),
                        frame.link_detail.c_str());
            std::fflush(stdout);
        }

        const std::int64_t now = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());

        for (const obsn::ObsEvent& event : client.drain_events()) {
            ++event_count;
            std::printf("event: %s\n", obsn::to_string(event.kind));
            // Run it through the real notification pipeline, so what is printed is the toast
            // the overlay would actually draw rather than a separate rendering of the event.
            notifications.submit(event, styling, frame.state, now);
            std::fflush(stdout);
        }
        for (const obsn::Notification& n : notifications.items()) {
            if (n.created_ms != now) continue;
            std::printf("  toast: \"%s\"%s%s\n", n.title.text.c_str(),
                        n.detail.text.empty() ? "" : "  /  ", n.detail.text.c_str());
            std::fflush(stdout);
        }
        notifications.tick(now, styling);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const obsn::LinkDiagnostics diagnostics = client.diagnostics();
    client.stop();

    std::printf("\nendpoint   %s\n", diagnostics.endpoint.c_str());
    std::printf("state      %s\n", obsn::to_string(diagnostics.state));
    if (!diagnostics.obs_version.empty()) {
        std::printf("obs        %s (%s)\n", diagnostics.obs_version.c_str(),
                    diagnostics.platform.c_str());
        std::printf("script     %s\n", diagnostics.script_version.c_str());
    }
    std::printf("messages   %llu received, %llu malformed\n",
                static_cast<unsigned long long>(diagnostics.messages_received),
                static_cast<unsigned long long>(diagnostics.malformed_messages));
    std::printf("events     %d\n", event_count);
    // A probe that never reached a handshake is a failure worth a non-zero exit, so CI and
    // shell scripts can act on it.
    return diagnostics.messages_received > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string command = argv[1];
    const std::vector<std::string> rest(argv + 2, argv + argc);

    if (command == "defaults") {
        std::cout << obsn::Config::defaults().serialise() << "\n";
        return 0;
    }
    if (command == "validate") {
        if (rest.empty()) return usage();
        return validate(rest);
    }
    if (command == "normalise" || command == "normalize") {
        if (rest.size() != 1) return usage();
        std::string text;
        if (!read_file(rest[0], text)) {
            std::fprintf(stderr, "%s: cannot be read\n", rest[0].c_str());
            return 1;
        }
        obsn::ConfigDiagnostics diag;
        std::cout << obsn::Config::parse(text, diag).serialise() << "\n";
        return 0;
    }
    if (command == "probe") {
        const int seconds = rest.empty() ? 10 : std::atoi(rest[0].c_str());
        const std::string endpoint = rest.size() > 1 ? rest[1] : std::string();
        return probe(seconds > 0 ? seconds : 10, endpoint);
    }
    if (command == "schema") return schema();
    if (command == "categories") return list_categories();
    return usage();
}
