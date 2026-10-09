// Records the public market WebSocket of a prediction-market order book to hourly files.
//
//   pm_record [--out DIR] [--tags a,b,c] [--per-tag N] [--min-days D] [--refresh-h H]
//             [--tokens-per-conn N] [--cap-gb G] [--seconds S]
//
// Markets are the top N by 24 h volume for each tag, re-chosen every H hours; the connections
// restart only when the chosen set changes. Every WebSocket text message is stored as one
// line "<receive time, ns since epoch> <message>" in DIR/c<k>/<UTC date>T<hour>.jsonl, which
// is compressed with zstd when the hour ends. DIR/events.log records connects, drops and
// gaps; DIR/markets.tsv maps token ids to markets. Recording stops at the size cap.

#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "net/tls.hpp"
#include "pm/gamma.hpp"

namespace fs = std::filesystem;
using hft::pm::kWsHost;
using hft::pm::kWsPath;
using hft::pm::Market;

namespace {

std::string utc(std::time_t t, const char* fmt) { return hft::pm::utc_time(t, fmt); }

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

std::int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// Sleeps up to `s` seconds, returning early when stop is set.
void nap(double s, const std::atomic<bool>& stop) {
    for (double t = 0; t < s && !stop && !g_stop; t += 0.1)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

class EventLog {
   public:
    explicit EventLog(const fs::path& p) : f_(p, std::ios::app) {}
    void write(const std::string& who, const std::string& what) {
        std::lock_guard<std::mutex> g(m_);
        f_ << now_ns() << ' ' << who << ' ' << what << '\n';
        f_.flush();
        std::fprintf(stderr, "%s %s %s\n", utc(std::time(nullptr), "%H:%M:%S").c_str(), who.c_str(), what.c_str());
    }

   private:
    std::mutex m_;
    std::ofstream f_;
};

std::uintmax_t dir_bytes(const fs::path& d) {
    std::uintmax_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(d, ec))
        if (e.is_regular_file(ec)) n += e.file_size(ec);
    return n;
}

void compress_file(const fs::path& raw) {
    std::ifstream in(raw, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (data.empty()) {
        fs::remove(raw);
        return;
    }
    std::string out(ZSTD_compressBound(data.size()), '\0');
    const std::size_t n = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 6);
    if (ZSTD_isError(n)) return;  // keep the raw file
    // A restart inside an hour leaves an earlier file for the same hour; keep both, in order.
    fs::path dst = raw;
    dst += ".zst";
    for (int k = 1; fs::exists(dst); ++k)
        dst = raw.parent_path() / (raw.stem().string() + "_" + std::to_string(k) + ".jsonl.zst");
    {
        std::ofstream o(dst, std::ios::binary | std::ios::trunc);
        o.write(out.data(), static_cast<std::streamsize>(n));
        if (!o) {
            fs::remove(dst);
            return;
        }
    }
    fs::remove(raw);
}

// Appends lines to the current hour's file and compresses it when the hour changes.
class HourlyWriter {
   public:
    HourlyWriter(fs::path dir, std::uintmax_t cap, const fs::path& root, std::atomic<bool>& cap_hit)
        : dir_(std::move(dir)), cap_(cap), root_(root), cap_hit_(cap_hit) {
        fs::create_directories(dir_);
    }
    ~HourlyWriter() { rotate(""); }

    void write(std::int64_t ns, const std::string& msg) {
        const std::string h = utc(static_cast<std::time_t>(ns / 1'000'000'000), "%Y%m%dT%H");
        if (h != hour_) {
            rotate(h);
            if (cap_hit_) return;
        }
        if (!f_.is_open()) return;
        f_ << ns << ' ';
        for (const char c : msg) f_ << (c == '\n' ? ' ' : c);
        f_ << '\n';
    }

   private:
    void rotate(const std::string& next) {
        if (f_.is_open()) {
            f_.close();
            compress_file(path_);
        }
        hour_ = next;
        if (next.empty()) return;
        if (dir_bytes(root_) >= cap_) {
            cap_hit_ = true;
            return;
        }
        path_ = dir_ / (next + ".jsonl");
        f_.open(path_, std::ios::app);
    }
    fs::path dir_, path_;
    std::uintmax_t cap_;
    fs::path root_;
    std::atomic<bool>& cap_hit_;
    std::string hour_;
    std::ofstream f_;
};

struct Stats {
    std::atomic<std::uint64_t> msgs{0}, bytes{0}, reconnects{0};
    std::atomic<std::int64_t> last_rx_ns{0};
};

// Health file for monitoring: one JSON object, replaced atomically once a minute.
void write_status(const fs::path& root, const std::vector<std::unique_ptr<Stats>>& stats, std::size_t tokens) {
    std::ostringstream o;
    const std::int64_t now = now_ns();
    o << "{\"time_ns\":" << now << ",\"tokens\":" << tokens << ",\"disk_bytes\":" << dir_bytes(root)
      << ",\"connections\":[";
    for (std::size_t i = 0; i < stats.size(); ++i) {
        const Stats& s = *stats[i];
        const std::int64_t rx = s.last_rx_ns;
        o << (i ? "," : "") << "{\"messages\":" << s.msgs << ",\"bytes\":" << s.bytes << ",\"reconnects\":" << s.reconnects
          << ",\"last_message_age_s\":" << (rx ? static_cast<double>(now - rx) * 1e-9 : -1.0) << "}";
    }
    o << "]}\n";
    const fs::path tmp = root / "status.json.tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << o.str();
    }
    std::error_code ec;
    fs::rename(tmp, root / "status.json", ec);
}

void conn_loop(int idx, std::vector<std::string> tokens, const fs::path& root, std::uintmax_t cap,
               std::atomic<bool>& stop, std::atomic<bool>& cap_hit, EventLog& log, Stats& st) {
    const std::string who = "c" + std::to_string(idx);
    HourlyWriter w(root / who, cap, root, cap_hit);
    std::string sub = "{\"assets_ids\":[";
    for (std::size_t i = 0; i < tokens.size(); ++i) sub += (i ? ",\"" : "\"") + tokens[i] + "\"";
    sub += "],\"type\":\"market\",\"custom_feature_enabled\":true}";

    using Clock = std::chrono::steady_clock;
    double backoff = 1;
    while (!stop && !g_stop && !cap_hit) {
        try {
            hft::net::WsClient ws;
            ws.connect(kWsHost, kWsPath);
            ws.send_text(sub);
            log.write(who, "connected tokens=" + std::to_string(tokens.size()));
            backoff = 1;
            auto last_rx = Clock::now(), last_ping = last_rx;
            std::string msg;
            while (!stop && !g_stop && !cap_hit) {
                const auto s = ws.read(msg, 1000);
                const auto now = Clock::now();
                if (s == hft::net::WsClient::Status::Closed) {
                    log.write(who, "closed by server");
                    break;
                }
                if (s == hft::net::WsClient::Status::Message) {
                    last_rx = now;
                    if (msg != "PONG") {
                        w.write(now_ns(), msg);
                        st.msgs++;
                        st.last_rx_ns = now_ns();
                        st.bytes += msg.size();
                    }
                }
                if (now - last_ping >= std::chrono::seconds(10)) {
                    ws.send_text("PING");
                    last_ping = now;
                }
                if (now - last_rx > std::chrono::seconds(30)) {
                    log.write(who, "no data for 30 s, reconnecting");
                    break;
                }
            }
        } catch (const std::exception& e) {
            log.write(who, std::string("error: ") + e.what());
        }
        st.reconnects++;
        nap(backoff, stop);
        backoff = std::min(backoff * 2, 30.0);
    }
    log.write(who, cap_hit ? "stopped: size cap" : "stopped");
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string p; std::getline(ss, p, sep);)
        if (!p.empty()) out.push_back(p);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path root = "pm-data";
    std::vector<std::string> tags = {"sports", "politics", "crypto", "economy"};
    int per_tag = 8, min_days = 3, tokens_per_conn = 50;
    double refresh_h = 6, cap_gb = 50, seconds = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--out") root = val();
        else if (a == "--tags") tags = split(val(), ',');
        else if (a == "--per-tag") per_tag = std::atoi(val().c_str());
        else if (a == "--min-days") min_days = std::atoi(val().c_str());
        else if (a == "--refresh-h") refresh_h = std::atof(val().c_str());
        else if (a == "--tokens-per-conn") tokens_per_conn = std::max(1, std::atoi(val().c_str()));
        else if (a == "--cap-gb") cap_gb = std::atof(val().c_str());
        else if (a == "--seconds") seconds = std::atof(val().c_str());
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    fs::create_directories(root);
    EventLog log(root / "events.log");
    const auto cap = static_cast<std::uintmax_t>(cap_gb * 1e9);

    // A crash can leave an uncompressed hour behind.
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file() && e.path().extension() == ".jsonl") compress_file(e.path());

    std::atomic<bool> cap_hit{false};
    const auto t_start = std::chrono::steady_clock::now();
    std::vector<std::string> current;
    std::atomic<bool> stop_conns{false};
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<Stats>> stats;
    auto stop_all = [&] {
        stop_conns = true;
        for (auto& t : threads) t.join();
        threads.clear();
        stats.clear();
        stop_conns = false;
    };

    while (!g_stop && !cap_hit) {
        std::vector<Market> markets;
        try {
            markets = hft::pm::discover_markets(tags, per_tag, min_days);
        } catch (const std::exception& e) {
            log.write("main", std::string("discovery failed: ") + e.what());
            if (current.empty()) {
                nap(30, stop_conns);
                continue;
            }
        }
        if (!markets.empty()) {
            std::set<std::string> want;
            {
                std::ofstream tsv(root / "markets.tsv", std::ios::app);
                for (const Market& m : markets)
                    for (const auto& [tok, name] : m.tokens) {
                        want.insert(tok);
                        tsv << utc(std::time(nullptr), "%Y-%m-%dT%H:%M:%SZ") << '\t' << tok << '\t' << m.condition
                            << '\t' << m.tag << '\t' << m.slug << '\t' << m.end << '\t' << name << '\n';
                    }
            }
            std::vector<std::string> next(want.begin(), want.end());
            if (next != current) {
                stop_all();
                current = next;
                log.write("main", "markets=" + std::to_string(markets.size()) + " tokens=" + std::to_string(current.size()));
                int idx = 0;
                for (std::size_t b = 0; b < current.size(); b += static_cast<std::size_t>(tokens_per_conn), ++idx) {
                    std::vector<std::string> chunk(current.begin() + static_cast<std::ptrdiff_t>(b),
                                                   current.begin() + static_cast<std::ptrdiff_t>(std::min(
                                                                         current.size(), b + static_cast<std::size_t>(tokens_per_conn))));
                    stats.push_back(std::make_unique<Stats>());
                    threads.emplace_back(conn_loop, idx, std::move(chunk), std::cref(root), cap, std::ref(stop_conns),
                                         std::ref(cap_hit), std::ref(log), std::ref(*stats.back()));
                }
            }
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(refresh_h * 3600);
        auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!g_stop && !cap_hit && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto now = std::chrono::steady_clock::now();
            if (seconds > 0 && std::chrono::duration<double>(now - t_start).count() >= seconds) g_stop = true;
            if (now >= next_report) {
                next_report = now + std::chrono::seconds(60);
                std::uint64_t m = 0, b = 0;
                for (auto& s : stats) m += s->msgs, b += s->bytes;
                log.write("main", "msgs=" + std::to_string(m) + " bytes=" + std::to_string(b) +
                                      " disk=" + std::to_string(dir_bytes(root) >> 20) + "MiB");
                write_status(root, stats, current.size());
            }
        }
    }
    {
        std::uint64_t m = 0, b = 0;
        for (auto& s : stats) m += s->msgs, b += s->bytes;
        stop_all();
        log.write("main", "exit msgs=" + std::to_string(m) + " bytes=" + std::to_string(b));
    }
    return cap_hit ? 3 : 0;
}
