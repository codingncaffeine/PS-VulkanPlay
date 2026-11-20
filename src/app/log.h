// The application log: every line goes to stderr and, once a file is open, to
// that file as well. The file is the whole record: it also takes the lines below
// the level stderr shows, and what a command prints. Safe to call from any thread.
#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace psvp::log {

enum class Level { debug, info, warn, error };

void set_level(Level minimum);

// Opens the log file, keeping the previous run's file beside it as "<name>.1".
// Returns false (and keeps logging to stderr) when the file cannot be opened.
bool open_file(const std::filesystem::path& file);

void close_file();

void write(Level level, std::string_view text);
// A line a command printed itself: into the file only, when one is open.
void record(std::string_view text);

template <class... Args>
void debug(std::format_string<Args...> format, Args&&... args) {
    write(Level::debug, std::format(format, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> format, Args&&... args) {
    write(Level::info, std::format(format, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> format, Args&&... args) {
    write(Level::warn, std::format(format, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> format, Args&&... args) {
    write(Level::error, std::format(format, std::forward<Args>(args)...));
}

}  // namespace psvp::log
