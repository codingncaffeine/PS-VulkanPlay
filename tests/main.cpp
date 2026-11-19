#include <cstdio>
#include <string_view>
#include <vector>

#include "check.h"

namespace psvp::test {
namespace {

struct Case {
    const char* name;
    void (*run)();
};

std::vector<Case>& cases() {
    // Never destroyed, so it is still there for anything that runs at exit.
    static auto* list = new std::vector<Case>();
    return *list;
}

int g_checks = 0;
int g_failed_checks = 0;

}  // namespace

Registrar::Registrar(const char* name, void (*run)()) { cases().push_back(Case{name, run}); }

void record(bool passed, std::string_view expression, const std::source_location& where) {
    ++g_checks;
    if (passed) return;
    ++g_failed_checks;
    std::fprintf(stderr, "    CHECK FAILED %s:%u: %.*s\n", where.file_name(), static_cast<unsigned>(where.line()),
                 static_cast<int>(expression.size()), expression.data());
}

int run_all(int argc, char** argv) {
    const std::string_view filter = argc > 1 ? std::string_view(argv[1]) : std::string_view();
    int ran = 0;
    int failed_tests = 0;
    for (const Case& test : cases()) {
        if (!filter.empty() && std::string_view(test.name).find(filter) == std::string_view::npos) continue;
        const int before = g_failed_checks;
        test.run();
        ++ran;
        const bool ok = g_failed_checks == before;
        if (!ok) ++failed_tests;
        std::fprintf(stderr, "  %s  %s\n", ok ? "ok  " : "FAIL", test.name);
    }
    std::fprintf(stderr, "%d tests, %d checks, %d failed\n", ran, g_checks, g_failed_checks);
    if (ran == 0) {
        std::fprintf(stderr, "no test ran\n");
        return 1;
    }
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace psvp::test

int main(int argc, char** argv) { return psvp::test::run_all(argc, argv); }
