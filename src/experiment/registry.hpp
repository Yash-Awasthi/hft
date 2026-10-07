#pragma once

// Experiment registry in SQLite: one row per run with its config hash, commit and data hash,
// so every result traces back to its inputs (N8) and the number of runs per research
// question feeds the deflated Sharpe ratio. Runs that open test days are flagged for the
// test-set lock audit.

#include <cstdint>
#include <string>
#include <vector>

#include "experiment/config.hpp"

struct sqlite3;

namespace hft::experiment {

struct TestAccess {
    std::int64_t run_id;
    std::string started;
    std::string question;
    std::string config_hash;
};

class Registry {
   public:
    explicit Registry(const std::string& path);
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // test_access: the run opens a test day (Splits::touches_test).
    std::int64_t begin(const RunConfig& c, const std::string& commit, const std::string& data_hash,
                       bool test_access);
    void finish(std::int64_t run, const std::string& metrics_json, const std::string& outputs);
    std::int64_t count(const std::string& question) const;
    std::vector<TestAccess> test_accesses() const;

   private:
    void exec(const char* sql) const;
    sqlite3* db_ = nullptr;
};

}  // namespace hft::experiment
