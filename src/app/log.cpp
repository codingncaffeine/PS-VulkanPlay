#include "app/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <system_error>

namespace psvp::log {
namespace {

struct State {
    std::mutex mutex;
    Level minimum = Level::info;
    std::FILE* file = nullptr;
};

State& state() {
    // Never destroyed: a line logged while the program is shutting down still has a mutex to take.
    static State* instance = new State();
    return *instance;
}

std::string_view label(Level level) {
    switch (level) {
        case Level::debug: return "DEBUG";
        case Level::info: return "INFO ";
        case Level::warn: return "WARN ";
        case Level::error: return "ERROR";
    }
    return "?????";
}

std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm utc{};
    ::gmtime_r(&seconds, &utc);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", utc.tm_year + 1900, utc.tm_mon + 1,
                       utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
}

}  // namespace

void set_level(Level minimum) {
    State& s = state();
    const std::lock_guard lock(s.mutex);
    s.minimum = minimum;
}

bool open_file(const std::filesystem::path& file) {
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    std::filesystem::path previous = file;
    previous += ".1";
    std::filesystem::rename(file, previous, ignored);  // no file yet is fine

    std::FILE* opened = std::fopen(file.c_str(), "w");
    State& s = state();
    const std::lock_guard lock(s.mutex);
    if (s.file != nullptr) std::fclose(s.file);
    s.file = opened;
    return opened != nullptr;
}

void close_file() {
    State& s = state();
    const std::lock_guard lock(s.mutex);
    if (s.file != nullptr) std::fclose(s.file);
    s.file = nullptr;
}

void write(Level level, std::string_view text) {
    State& s = state();
    const std::lock_guard lock(s.mutex);
    if (level < s.minimum && s.file == nullptr) return;
    const std::string line = std::format("{} {} {}\n", timestamp(), label(level), text);
    if (level >= s.minimum) std::fputs(line.c_str(), stderr);
    if (s.file != nullptr) {
        std::fputs(line.c_str(), s.file);
        std::fflush(s.file);
    }
}

void record(std::string_view text) {
    State& s = state();
    const std::lock_guard lock(s.mutex);
    if (s.file == nullptr) return;
    const std::string line = std::format("{} SAID  {}\n", timestamp(), text);
    std::fputs(line.c_str(), s.file);
    std::fflush(s.file);
}

}  // namespace psvp::log
