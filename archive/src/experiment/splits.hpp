#pragma once

// Day split registry (configs/splits.toml). Test days are locked: a run that includes one
// must say allow_test, and the registry logs it.

#include <map>
#include <string>
#include <string_view>

#include "experiment/config.hpp"

namespace hft::experiment {

enum class Split { Train, Validation, Test, Robustness };

class Splits {
   public:
    Split of(const std::string& day) const;
    bool touches_test(const RunConfig& c) const;
    // Throws ConfigError when the run includes a locked test day without allow_test.
    void check_access(const RunConfig& c) const;
    const std::map<std::string, std::string>& files() const { return files_; }

   private:
    friend Splits parse_splits(std::string_view);
    std::map<std::string, Split> days_;
    std::map<std::string, std::string> files_;
    bool test_locked_ = true;
};

Splits parse_splits(std::string_view toml);
Splits load_splits(const std::string& path);

}  // namespace hft::experiment
