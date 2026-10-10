#pragma once

// Command-line checks shared by the apps. Each prints the problem and the usage line and returns
// false; the caller exits 2.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>

namespace hft::app {

// An argument that looks like an option ("-x", "--x") must be one of `options`.
inline bool known_options(int argc, char** argv, std::initializer_list<const char*> options, const char* usage) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' || !argv[i][1]) continue;
        bool ok = false;
        for (const char* o : options) ok = ok || std::strcmp(argv[i], o) == 0;
        if (!ok) return std::fprintf(stderr, "unknown option %s\n%s\n", argv[i], usage), false;
    }
    return true;
}

inline bool is_dir(const char* p, const char* usage) {
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) return true;
    return std::fprintf(stderr, "not a directory: %s\n%s\n", p, usage), false;
}

}  // namespace hft::app
