#pragma once

// What a pm_live run subscribed to, and its recording. The session file lists tokens (with
// connection and label) and arbitrage groups, so a replay sets the engine up the same way;
// the recorder writes the records the trading thread processed, in its order.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/spsc.hpp"
#include "pm/engine.hpp"
#include "pm/gamma.hpp"
#include "pm/reader.hpp"

namespace hft::pm {

// Tokens, their connections and labels, and the arbitrage groups; written next to a
// recording so a replay sets the engine up the same way.
struct Session {
    struct Tok {
        std::string id, label;
        int conn = 0;
        MarketRules rules;
    };
    struct Group {
        std::string name;
        std::vector<std::string> ids;
    };
    std::vector<Tok> tokens;
    std::vector<Group> groups;

    // Tabs and newlines in names would split fields, so they are written as spaces.
    static std::string field(std::string s) {
        for (char& c : s)
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return s;
    }

    void save(const std::filesystem::path& p) const {
        std::ofstream f(p);
        for (const Tok& t : tokens) {
            const MarketRules& r = t.rules;
            f << "token\t" << t.id << '\t' << t.conn << '\t' << field(t.label) << '\t' << r.tick << '\t' << r.min_qty << '\t'
              << r.neg_risk << '\t' << r.fees << '\t' << r.fee_rate_ppm << '\t' << int{r.fee_exp} << '\t' << r.delay_ms << '\n';
        }
        for (const Group& g : groups) {
            f << "group\t" << field(g.name);
            for (const auto& id : g.ids) f << '\t' << id;
            f << '\n';
        }
    }
    static Session load(const std::filesystem::path& p) {
        Session s;
        std::ifstream f(p);
        if (!f) throw std::runtime_error("cannot read " + p.string());
        for (std::string line; std::getline(f, line);) {
            std::vector<std::string> c;
            std::stringstream ss(line);
            for (std::string x; std::getline(ss, x, '\t');) c.push_back(x);
            if (c.size() >= 4 && c[0] == "token") {
                Tok t{c[1], c[3], std::atoi(c[2].c_str()), {}};
                if (c.size() >= 11) {  // files written before the rules were kept have 4 fields
                    MarketRules& r = t.rules;
                    r.tick = std::atoi(c[4].c_str());
                    r.min_qty = std::atoll(c[5].c_str());
                    r.neg_risk = c[6] == "1";
                    r.fees = c[7] == "1";
                    r.fee_rate_ppm = static_cast<std::uint32_t>(std::atol(c[8].c_str()));
                    r.fee_exp = static_cast<std::uint8_t>(std::atoi(c[9].c_str()));
                    r.delay_ms = static_cast<std::uint16_t>(std::atoi(c[10].c_str()));
                }
                s.tokens.push_back(std::move(t));
            }
            else if (c.size() >= 3 && c[0] == "group") s.groups.push_back({c[1], {c.begin() + 2, c.end()}});
        }
        return s;
    }
    void apply(Engine& e) const {
        for (const Tok& t : tokens) e.set_conn(e.token(t.id), static_cast<std::uint32_t>(t.conn));
        for (const Group& g : groups) e.add_group(g.name, g.ids);
    }
};

// Writes every record the trading thread processed, in its order, as "<ns> <json>" lines in
// hourly files (by receive time) under DIR/c0, compressing each when its hour ends. One
// connection directory, so a replay sees exactly the live order. Writing stops, and trading
// goes on, once the directory reaches the size cap, or once the ring fills because the disk
// fell behind: the trading thread never waits, and what was written stays a valid prefix.
class Recorder {
   public:
    Recorder(const std::filesystem::path& dir, double cap_gb, std::size_t ring_bytes = 1 << 24)
        : dir_(dir), cap_(static_cast<std::uintmax_t>(cap_gb * 1e9)), ring_(ring_bytes) {
        std::filesystem::create_directories(dir / "c0");
        thread_ = std::thread([this] { run(); });
    }
    ~Recorder() {
        done_ = true;
        thread_.join();
        rotate(-1);
    }
    void write(std::int64_t ns, std::string_view text) {
        if (overflowed_ || ring_.try_write(ns, 0, 0, text)) return;
        overflowed_ = true;
        std::fprintf(stderr, "recording stopped: the recorder fell behind\n");
    }
    bool capped() const { return capped_; }
    bool overflowed() const { return overflowed_; }

   private:
    void run() {
        SpscBytes::Rec r;
        for (;;) {
            const bool last = done_;  // read before draining, so nothing written after it is missed
            while (ring_.try_read(r)) {
                const std::int64_t hour = r.ns / 3'600'000'000'000;
                if (hour != hour_) rotate(hour);
                if (f_) {
                    std::fprintf(f_, "%lld ", static_cast<long long>(r.ns));
                    std::fwrite(r.data.data(), 1, r.data.size(), f_);
                    std::fputc('\n', f_);
                }
                ring_.release();
            }
            if (last) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    void rotate(std::int64_t hour) {
        if (f_) {
            std::fclose(f_);
            f_ = nullptr;
            compress_record_file(path_);
        }
        hour_ = hour;
        if (hour < 0 || capped_) return;
        std::uintmax_t used = 0;
        std::error_code ec;
        for (const auto& e : std::filesystem::recursive_directory_iterator(dir_, ec))
            if (e.is_regular_file(ec)) used += e.file_size(ec);
        if (used >= cap_) {
            capped_ = true;
            std::fprintf(stderr, "recording stopped at the size cap\n");
            return;
        }
        path_ = dir_ / "c0" / (utc_time(static_cast<std::time_t>(hour * 3600), "%Y%m%dT%H") + ".jsonl");
        f_ = std::fopen(path_.c_str(), "ab");
        if (!f_) throw std::runtime_error("cannot write " + path_.string());
        std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
    }


    std::filesystem::path dir_, path_;
    std::uintmax_t cap_;
    SpscBytes ring_;
    std::FILE* f_ = nullptr;
    std::int64_t hour_ = -1;
    std::atomic<bool> done_{false};
    std::atomic<bool> capped_{false};
    bool overflowed_ = false;  // trading thread only
    std::thread thread_;
};

}  // namespace hft::pm
