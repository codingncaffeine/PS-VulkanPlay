// A small test harness: PSVP_TEST(name) { CHECK(condition); }
// Failures go to stderr with the file and line; the program exits 1 if any check failed.
#pragma once

#include <source_location>
#include <string_view>

namespace psvp::test {

struct Registrar {
    Registrar(const char* name, void (*run)());
};

void record(bool passed, std::string_view expression, const std::source_location& where);

// Runs every registered test (or only those whose name contains argv[1]) and returns the exit code.
int run_all(int argc, char** argv);

}  // namespace psvp::test

#define PSVP_TEST(name)                                                    \
    static void name();                                                    \
    static const ::psvp::test::Registrar psvp_registrar_##name(#name, &name); \
    static void name()

#define CHECK(condition) \
    ::psvp::test::record(static_cast<bool>(condition), #condition, std::source_location::current())
