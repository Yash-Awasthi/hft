#pragma once

#include <unistd.h>

#include <filesystem>
#include <string>

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& tag)
        : path(std::filesystem::temp_directory_path() /
               ("hft_" + tag + "_" + std::to_string(::getpid()))) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};
