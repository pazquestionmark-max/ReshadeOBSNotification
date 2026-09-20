// SPDX-License-Identifier: MIT
// Structured logging with configurable verbosity and a hard size cap.
//
// Two rules this module enforces rather than merely documents:
//   * chat message content is never written unless include_message_content is explicitly on;
//   * the file is capped and rotated once, so a long session cannot fill the user's disk.
#ifndef OBSN_LOG_HPP
#define OBSN_LOG_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace obsn {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

LogLevel parse_log_level(std::string_view, LogLevel fallback = LogLevel::Info) noexcept;
const char* to_string(LogLevel) noexcept;

struct LogEntry {
    LogLevel level = LogLevel::Info;
    std::int64_t timestamp_ms = 0;
    std::string component;
    std::string message;
};

/// Thread-safe. Writes to an optional file and always keeps the last `ring_capacity` entries in
/// memory so the Diagnostics tab can show recent activity without reading back from disk.
class Logger {
public:
    static Logger& instance();

    void configure(LogLevel level, bool to_file, std::string file_path, int max_file_kb);
    void set_level(LogLevel l);
    LogLevel level() const;
    /// True only when the user explicitly opted in to logging message content.
    void set_include_message_content(bool v);
    bool include_message_content() const;

    void write(LogLevel level, std::string_view component, std::string_view message);

    std::vector<LogEntry> recent(std::size_t max_entries = 100) const;
    const std::string& file_path() const;
    /// Bytes written to the current file since it was opened or last rotated.
    std::uint64_t bytes_written() const;

private:
    Logger() = default;
    void rotate_locked();

    mutable std::mutex mutex_;
    LogLevel level_ = LogLevel::Info;
    bool to_file_ = false;
    bool include_content_ = false;
    std::string file_path_;
    std::uint64_t max_bytes_ = 1024ull * 1024ull;
    std::uint64_t bytes_ = 0;
    std::vector<LogEntry> ring_;
    std::size_t ring_next_ = 0;
    std::size_t ring_capacity_ = 256;
};

void log_write(LogLevel level, std::string_view component, std::string_view message);

#define OBSN_LOG(level, component, message) ::obsn::log_write((level), (component), (message))
#define OBSN_TRACE(c, m) OBSN_LOG(::obsn::LogLevel::Trace, c, m)
#define OBSN_DEBUG(c, m) OBSN_LOG(::obsn::LogLevel::Debug, c, m)
#define OBSN_INFO(c, m) OBSN_LOG(::obsn::LogLevel::Info, c, m)
#define OBSN_WARN(c, m) OBSN_LOG(::obsn::LogLevel::Warn, c, m)
#define OBSN_ERROR(c, m) OBSN_LOG(::obsn::LogLevel::Error, c, m)

}  // namespace obsn

#endif  // OBSN_LOG_HPP
