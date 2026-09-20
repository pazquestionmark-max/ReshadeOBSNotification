// SPDX-License-Identifier: MIT
#include "obsn/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <algorithm>

namespace obsn {
namespace {

std::int64_t now_ms() {  // NOLINT: wall-clock time is what a log line needs
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string format_timestamp(std::int64_t ms) {
    const std::time_t secs = static_cast<std::time_t>(ms / 1000);
    const int millis = static_cast<int>(ms % 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
    return std::string(buf);
}

}  // namespace

LogLevel parse_log_level(std::string_view s, LogLevel fallback) noexcept {
    if (s == "trace") return LogLevel::Trace;
    if (s == "debug") return LogLevel::Debug;
    if (s == "info") return LogLevel::Info;
    if (s == "warn" || s == "warning") return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    if (s == "off" || s == "none") return LogLevel::Off;
    return fallback;
}

const char* to_string(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off: return "OFF";
    }
    return "INFO";
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::configure(LogLevel level, bool to_file, std::string file_path, int max_file_kb) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
    to_file_ = to_file && !file_path.empty();
    file_path_ = std::move(file_path);
    max_bytes_ = static_cast<std::uint64_t>(max_file_kb > 0 ? max_file_kb : 1024) *
                 static_cast<std::uint64_t>(1024);
    bytes_ = 0;
    if (ring_.size() != ring_capacity_) {
        ring_.assign(ring_capacity_, LogEntry{});
        ring_next_ = 0;
    }
    if (to_file_) {
        // Truncate on configure so each run starts with its own log rather than appending to an
        // arbitrarily old one.
        std::ofstream truncate(file_path_, std::ios::binary | std::ios::trunc);
    }
}

void Logger::set_level(LogLevel l) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = l;
}

LogLevel Logger::level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

void Logger::set_include_message_content(bool v) {
    std::lock_guard<std::mutex> lock(mutex_);
    include_content_ = v;
}

bool Logger::include_message_content() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return include_content_;
}

const std::string& Logger::file_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_path_;
}

std::uint64_t Logger::bytes_written() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

void Logger::rotate_locked() {
    // One generation of history: <file>.1 is replaced, never accumulated.
    const std::string backup = file_path_ + ".1";
    std::remove(backup.c_str());
    std::rename(file_path_.c_str(), backup.c_str());
    bytes_ = 0;
}

void Logger::write(LogLevel level, std::string_view component, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level_ == LogLevel::Off || level < level_) return;

    LogEntry entry;
    entry.level = level;
    entry.timestamp_ms = now_ms();
    entry.component = std::string(component);
    entry.message = std::string(message);

    if (ring_.size() != ring_capacity_) ring_.assign(ring_capacity_, LogEntry{});
    ring_[ring_next_] = entry;
    ring_next_ = (ring_next_ + 1) % ring_capacity_;

    if (!to_file_) return;
    if (bytes_ >= max_bytes_) rotate_locked();

    std::string level_field = to_string(level);
    level_field.resize(5, ' ');  // fixed-width so the columns line up when read back

    std::string line;
    line.reserve(entry.component.size() + message.size() + 48);
    line += format_timestamp(entry.timestamp_ms);
    line += " [";
    line += level_field;
    line += "] ";
    line += entry.component;
    line += ": ";
    line.append(message);
    line += '\n';

    std::ofstream out(file_path_, std::ios::binary | std::ios::app);
    if (!out) return;
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    if (out) bytes_ += line.size();
}

std::vector<LogEntry> Logger::recent(std::size_t max_entries) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LogEntry> out;
    if (ring_.empty()) return out;
    const std::size_t count = std::min(max_entries, ring_capacity_);
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t idx = (ring_next_ + ring_capacity_ - count + i) % ring_capacity_;
        if (ring_[idx].timestamp_ms != 0) out.push_back(ring_[idx]);
    }
    return out;
}

void log_write(LogLevel level, std::string_view component, std::string_view message) {
    Logger::instance().write(level, component, message);
}

}  // namespace obsn
