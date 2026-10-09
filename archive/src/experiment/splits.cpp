#include "experiment/splits.hpp"

#include <fstream>
#include <sstream>
#include <toml++/toml.hpp>

namespace hft::experiment {

Split Splits::of(const std::string& day) const {
    const auto it = days_.find(day);
    if (it == days_.end()) throw ConfigError("splits: day " + day + " is not registered");
    return it->second;
}

bool Splits::touches_test(const RunConfig& c) const {
    for (const auto& d : c.days)
        if (of(d) == Split::Test) return true;
    return false;
}

void Splits::check_access(const RunConfig& c) const {
    if (test_locked_ && touches_test(c) && !c.allow_test)
        throw ConfigError("splits: run " + c.name + " opens a locked test day without allow_test");
}

Splits parse_splits(std::string_view text) {
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error& e) {
        throw ConfigError("splits: TOML syntax: " + std::string(e.description()));
    }
    Splits s;
    const std::pair<const char*, Split> groups[] = {{"train", Split::Train},
                                                    {"validation", Split::Validation},
                                                    {"test", Split::Test},
                                                    {"robustness", Split::Robustness}};
    for (const auto& [name, split] : groups) {
        const toml::array* days = root.at_path(std::string(name) + ".days").as_array();
        if (!days) throw ConfigError(std::string("splits: missing ") + name + ".days");
        for (const auto& d : *days) {
            if (!d.is_string()) throw ConfigError("splits: days must be strings");
            if (!s.days_.emplace(d.as_string()->get(), split).second)
                throw ConfigError("splits: day " + d.as_string()->get() + " is in two groups");
        }
    }
    s.test_locked_ = root.at_path("test.locked").value_or(true);
    if (const toml::table* f = root["files"].as_table())
        for (const auto& [day, file] : *f)
            if (file.is_string()) s.files_[std::string(day.str())] = file.as_string()->get();
    return s;
}

Splits load_splits(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw ConfigError("splits: cannot open " + path);
    std::ostringstream s;
    s << f.rdbuf();
    return parse_splits(s.str());
}

}  // namespace hft::experiment
