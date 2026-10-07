#pragma once

// Run configuration: one TOML file per run, validated against a fixed schema before
// anything starts, and hashed in a canonical form so the registry can tell runs apart.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "engine/matching.hpp"
#include "engine/replay_exchange.hpp"

namespace hft::experiment {

struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct RunConfig {
    std::string name;
    std::string question;  // research question the run counts towards (deflated Sharpe)
    std::uint64_t seed = 0;
    std::vector<std::string> days;  // YYYY-MM-DD
    std::vector<std::string> symbols;
    bool allow_test = false;  // the only way to open a locked test day
    engine::Config exchange;
    engine::FillRule fill_rule = engine::FillRule::Queue;
    std::uint64_t market_data_ns = 0, order_entry_ns = 0, processing_ns = 0;
    std::string strategy;
    std::string strategy_params;  // canonical TOML of [strategy.params]
    std::string canonical;        // every field, defaults included, in a fixed order
    std::string hash;             // SHA-256 of canonical
};

RunConfig parse_config(std::string_view toml);
RunConfig load_config(const std::string& path);

}  // namespace hft::experiment
