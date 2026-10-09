// Live paper trading on the prediction-market order-book stream, and replay of its recordings.
//
//   pm_live [--events N] [--max-tokens N] [--tokens-per-conn N] [--port P] [--record DIR [--record-cap-gb G]]
//           [--seconds S] [--cpu C] [--spin] [--no-quote] [maker and risk options]
//   pm_live --replay DIR [maker and risk options]
//
// Live: picks the most traded events in which exactly one market resolves Yes and every open
// market trades, subscribes to all their tokens, and runs one thread per WebSocket connection
// (receive, timestamp, copy into a lock-free byte ring), one trading thread (parse, books,
// logit Avellaneda-Stoikov makers with paper fills against live trades, portfolio risk,
// arbitrage scanner), one recording thread and one HTTP thread on 127.0.0.1 serving
// /metrics (Prometheus text), /metrics.json and a dashboard at /. No order is ever sent.
// An idle trading thread sleeps on a futex that the feed threads ring; --spin busy-polls
// instead (lowest latency, one core at 100%), best combined with --cpu.
//
// Replay: runs the same trading code on a recording made with --record and prints the same
// summary; identical input gives identical decisions (the summary ends with their hash).
//
// Maker and risk options: --gamma G --k K --size S --max-inv N --horizon-s T
//                         --loss-stop USD --max-gross N --stale-s S

#include <pthread.h>
#include <sched.h>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/doorbell.hpp"
#include "core/histogram.hpp"
#include "core/spsc.hpp"
#include "core/tsc.hpp"
#include "net/http_server.hpp"
#include "net/tls.hpp"
#include "pm/dashboard.hpp"
#include "pm/engine.hpp"
#include "pm/gamma.hpp"
#include "pm/reader.hpp"

namespace fs = std::filesystem;
using namespace hft;
using namespace hft::pm;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<int> g_signal{0};
void on_signal(int sig) { g_signal = sig, g_stop = true; }

std::int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

std::string control(const char* type, int conn) {
    return std::string("{\"event_type\":\"") + type + "\",\"conn\":" + std::to_string(conn) + "}";
}

struct Options {
    EngineParams engine;
    int events = 40, max_tokens = 200, tokens_per_conn = 50, port = 8088, cpu = -1;
    bool spin = false;  // busy-poll the rings instead of sleeping on the doorbell
    double seconds = 0, record_cap_gb = 20;
    fs::path record, replay;
};

// ---------------------------------------------------------------- session description

// Tokens, their connections and labels, and the arbitrage groups; written next to a
// recording so a replay sets the engine up the same way.
struct Session {
    struct Tok {
        std::string id, label;
        int conn = 0;
    };
    struct Group {
        std::string name;
        std::vector<std::string> ids;
    };
    std::vector<Tok> tokens;
    std::vector<Group> groups;

    void save(const fs::path& p) const {
        std::ofstream f(p);
        for (const Tok& t : tokens) f << "token\t" << t.id << '\t' << t.conn << '\t' << t.label << '\n';
        for (const Group& g : groups) {
            f << "group\t" << g.name;
            for (const auto& id : g.ids) f << '\t' << id;
            f << '\n';
        }
    }
    static Session load(const fs::path& p) {
        Session s;
        std::ifstream f(p);
        if (!f) throw std::runtime_error("cannot read " + p.string());
        for (std::string line; std::getline(f, line);) {
            std::vector<std::string> c;
            std::stringstream ss(line);
            for (std::string x; std::getline(ss, x, '\t');) c.push_back(x);
            if (c.size() >= 4 && c[0] == "token") s.tokens.push_back({c[1], c[3], std::atoi(c[2].c_str())});
            else if (c.size() >= 3 && c[0] == "group") s.groups.push_back({c[1], {c.begin() + 2, c.end()}});
        }
        return s;
    }
    void apply(Engine& e) const {
        for (const Tok& t : tokens) e.set_conn(e.token(t.id), static_cast<std::uint32_t>(t.conn));
        for (const Group& g : groups) e.add_group(g.name, g.ids);
    }
};

Session discover(const Options& o) {
    Session s;
    int taken = 0;
    for (const EventInfo& ev : discover_events(o.events)) {
        if (!ev.complete) continue;
        std::vector<const Market*> open;
        std::size_t n = 0;
        for (const Market& m : ev.markets)
            if (m.open && m.tokens.size() == 2) open.push_back(&m), n += 2;
        if (open.size() < 2 || static_cast<int>(s.tokens.size() + n) > o.max_tokens) continue;
        Session::Group all{ev.slug, {}};
        for (const Market* m : open) {
            for (const auto& [id, outcome] : m->tokens) s.tokens.push_back({id, (m->title.empty() ? m->slug : m->title) + " " + outcome, 0});
            all.ids.push_back(m->tokens[0].first);  // Yes
            s.groups.push_back({m->slug, {m->tokens[0].first, m->tokens[1].first}});
        }
        s.groups.push_back(std::move(all));
        ++taken;
    }
    for (std::size_t i = 0; i < s.tokens.size(); ++i) s.tokens[i].conn = static_cast<int>(i) / o.tokens_per_conn;
    std::fprintf(stderr, "events %d tokens %zu groups %zu\n", taken, s.tokens.size(), s.groups.size());
    return s;
}

// ---------------------------------------------------------------- feed threads

struct Feed {
    Feed(int i, Doorbell& b) : idx(i), bell(b), ring(1 << 23) {}
    int idx;
    Doorbell& bell;
    std::vector<std::string> ids;
    SpscBytes ring;
    std::atomic<std::uint64_t> msgs{0}, bytes{0}, reconnects{0}, drops{0};
    std::atomic<std::int64_t> last_rx_ns{0};
};

bool put(Feed& f, std::string_view text) {
    if (f.ring.try_write(now_ns(), tsc::start(), static_cast<std::uint32_t>(f.idx), text)) {
        f.bell.ring();
        return true;
    }
    ++f.drops;
    return false;
}

void feed_loop(Feed& f) {
    std::string sub = "{\"assets_ids\":[";
    for (std::size_t i = 0; i < f.ids.size(); ++i) sub += (i ? ",\"" : "\"") + f.ids[i] + "\"";
    sub += "],\"type\":\"market\",\"custom_feature_enabled\":true}";
    const std::string beat = control("_heartbeat", f.idx), lost = control("_reconnect", f.idx);
    using Clock = std::chrono::steady_clock;
    double backoff = 1;
    bool first = true;
    while (!g_stop) {
        try {
            net::WsClient ws;
            ws.connect(kWsHost, kWsPath);
            ws.send_text(sub);
            if (!first) {
                ++f.reconnects;
                while (!put(f, lost) && !g_stop) std::this_thread::yield();
            }
            first = false;
            backoff = 1;
            auto last_rx = Clock::now(), last_ping = last_rx, last_beat = last_rx;
            std::string msg;
            bool overflow = false;
            while (!g_stop && !overflow) {
                const auto s = ws.read(msg, 200);
                const auto now = Clock::now();
                if (s == net::WsClient::Status::Closed) break;
                if (s == net::WsClient::Status::Message) {
                    last_rx = now;
                    f.last_rx_ns = now_ns();
                    if (msg != "PONG") {
                        ++f.msgs, f.bytes += msg.size();
                        overflow = !put(f, msg);  // a lost message leaves books wrong: resubscribe
                    }
                }
                if (now - last_beat >= std::chrono::seconds(1) && now - last_rx < std::chrono::seconds(15)) {
                    put(f, beat);
                    last_beat = now;
                }
                if (now - last_ping >= std::chrono::seconds(10)) ws.send_text("PING"), last_ping = now;
                if (now - last_rx > std::chrono::seconds(30)) break;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "c%d: %s\n", f.idx, e.what());
        }
        for (double t = 0; t < backoff && !g_stop; t += 0.1) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        backoff = std::min(backoff * 2, 30.0);
    }
}

// ---------------------------------------------------------------- recording thread

// Writes every record the trading thread processed, in its order, as "<ns> <json>" lines in
// hourly files (by receive time) under DIR/c0, compressing each when its hour ends. One
// connection directory, so a replay sees exactly the live order. Writing stops, and trading
// goes on, once the directory reaches the size cap.
class Recorder {
   public:
    Recorder(const fs::path& dir, double cap_gb)
        : dir_(dir), cap_(static_cast<std::uintmax_t>(cap_gb * 1e9)), ring_(1 << 24) {
        fs::create_directories(dir / "c0");
        thread_ = std::thread([this] { run(); });
    }
    ~Recorder() {
        done_ = true;
        thread_.join();
        rotate(-1);
    }
    void write(std::int64_t ns, std::string_view text) {
        while (!ring_.try_write(ns, 0, 0, text)) std::this_thread::yield();
    }
    bool capped() const { return capped_; }

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
            compress(path_);
        }
        hour_ = hour;
        if (hour < 0 || capped_) return;
        std::uintmax_t used = 0;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(dir_, ec))
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
    static void compress(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string out(ZSTD_compressBound(data.size()), '\0');
        const std::size_t n = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 6);
        if (ZSTD_isError(n)) return;  // keep the raw file
        fs::path dst = p;
        dst += ".zst";
        for (int k = 1; fs::exists(dst); ++k)  // the same hour reopened after a clock step
            dst = p.parent_path() / (p.stem().string() + "_" + std::to_string(k) + ".jsonl.zst");
        std::ofstream(dst, std::ios::binary).write(out.data(), static_cast<std::streamsize>(n));
        fs::remove(p);
    }

    fs::path dir_, path_;
    std::uintmax_t cap_;
    SpscBytes ring_;
    std::FILE* f_ = nullptr;
    std::int64_t hour_ = -1;
    std::atomic<bool> done_{false};
    std::atomic<bool> capped_{false};
    std::thread thread_;
};

// ---------------------------------------------------------------- metrics

// Receive time minus the exchange's own stamp can be negative when the clocks disagree, so it
// is stored shifted by this much.
constexpr std::int64_t kExchShift = 10'000'000'000;

struct Latency {
    Histogram queue, parse, engine, total, exch;  // tsc ticks, except exch in ns plus kExchShift
    void reset() { queue.reset(), parse.reset(), engine.reset(), total.reset(), exch.reset(); }
};

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}

class Metrics {
   public:
    void publish(std::string text, std::string json) {
        std::lock_guard<std::mutex> g(m_);
        text_ = std::move(text), json_ = std::move(json);
    }
    std::string text() {
        std::lock_guard<std::mutex> g(m_);
        return text_;
    }
    std::string json() {
        std::lock_guard<std::mutex> g(m_);
        return json_;
    }

   private:
    std::mutex m_;
    std::string text_ = "# starting\n", json_ = "{}";
};

struct Snapshot {
    const Engine& e;
    const Session& s;
    const std::vector<std::unique_ptr<Feed>>& feeds;
    const Latency &window, &total;
    double tpn, uptime_s;
    std::uint64_t records;
};

std::string lat_json(const Histogram& h, double scale, double shift = 0) {
    auto v = [&](std::uint64_t x) { return fmt("%.0f", h.count() ? static_cast<double>(x) * scale - shift : 0.0); };
    return "{\"p50\":" + v(h.percentile(50)) + ",\"p99\":" + v(h.percentile(99)) + ",\"p999\":" + v(h.percentile(99.9)) +
           ",\"max\":" + v(h.max()) + ",\"n\":" + std::to_string(h.count()) + "}";
}

std::pair<std::string, std::string> render(const Snapshot& x) {
    const Engine& e = x.e;
    const double ns = 1 / x.tpn;
    std::uint64_t msgs = 0, bytes = 0, recon = 0, drops = 0;
    for (const auto& f : x.feeds) msgs += f->msgs, bytes += f->bytes, recon += f->reconnects, drops += f->drops;
    std::uint64_t windows = 0;
    for (const auto& g : e.arb().stats()) windows += g.windows;

    std::string t;
    auto line = [&](const std::string& k, double v) { t += k + " " + fmt("%.6g", v) + "\n"; };
    line("hft_uptime_seconds", x.uptime_s);
    line("hft_messages_total", static_cast<double>(msgs));
    line("hft_bytes_total", static_cast<double>(bytes));
    line("hft_records_total", static_cast<double>(x.records));
    line("hft_events_total", static_cast<double>(e.events()));
    line("hft_trades_total", static_cast<double>(e.trades()));
    line("hft_paper_fills_total", static_cast<double>(e.fills()));
    line("hft_reconnects_total", static_cast<double>(recon));
    line("hft_ring_drops_total", static_cast<double>(drops));
    line("hft_pnl_usd", e.pnl());
    line("hft_gross_shares", e.gross());
    line("hft_halted", e.halted());
    line("hft_arb_open_windows", static_cast<double>(e.arb().open_windows()));
    line("hft_arb_windows_total", static_cast<double>(windows));
    const std::pair<const char*, const Histogram*> stages[] = {
        {"queue", &x.total.queue}, {"parse", &x.total.parse}, {"engine", &x.total.engine}, {"wire_to_decision", &x.total.total}};
    for (const auto& [name, h] : stages)
        for (const double q : {0.5, 0.99, 0.999})
            line(std::string("hft_latency_ns{stage=\"") + name + "\",quantile=\"" + fmt("%g", q) + "\"}",
                 static_cast<double>(h->percentile(q * 100)) * ns);

    std::string j = "{\"uptime_s\":" + fmt("%.1f", x.uptime_s) + ",\"messages\":" + std::to_string(msgs) +
                    ",\"bytes\":" + std::to_string(bytes) + ",\"events\":" + std::to_string(e.events()) +
                    ",\"trades\":" + std::to_string(e.trades()) + ",\"fills\":" + std::to_string(e.fills()) +
                    ",\"reconnects\":" + std::to_string(recon) + ",\"drops\":" + std::to_string(drops) +
                    ",\"pnl\":" + fmt("%.2f", e.pnl()) + ",\"gross\":" + fmt("%.0f", e.gross()) +
                    ",\"halted\":" + (e.halted() ? "true" : "false") +
                    ",\"arb_open\":" + std::to_string(e.arb().open_windows()) + ",\"arb_windows\":" + std::to_string(windows);
    j += ",\"lat\":{\"queue\":" + lat_json(x.window.queue, ns) + ",\"parse\":" + lat_json(x.window.parse, ns) +
         ",\"engine\":" + lat_json(x.window.engine, ns) + ",\"total\":" + lat_json(x.window.total, ns) +
         ",\"exch\":" + lat_json(x.window.exch, 1, static_cast<double>(kExchShift)) + "}";
    j += ",\"conns\":[";
    const std::int64_t now = now_ns();
    for (std::size_t i = 0; i < x.feeds.size(); ++i) {
        const Feed& f = *x.feeds[i];
        const std::int64_t rx = f.last_rx_ns;
        j += (i ? "," : "") + std::string("{\"tokens\":") + std::to_string(f.ids.size()) + ",\"messages\":" +
             std::to_string(f.msgs) + ",\"reconnects\":" + std::to_string(f.reconnects) +
             ",\"age_s\":" + fmt("%.1f", rx ? static_cast<double>(now - rx) * 1e-9 : -1.0) + "}";
    }
    j += "],\"groups\":[";
    std::vector<std::size_t> gi(e.arb().stats().size());
    for (std::size_t i = 0; i < gi.size(); ++i) gi[i] = i;
    const auto& gs = e.arb().stats();
    std::sort(gi.begin(), gi.end(), [&](std::size_t a, std::size_t b) {
        return gs[a].windows != gs[b].windows ? gs[a].windows > gs[b].windows : gs[a].name < gs[b].name;
    });
    for (std::size_t k = 0; k < gi.size() && k < 25; ++k) {
        const auto& g = gs[gi[k]];
        j += (k ? "," : "") + std::string("{\"name\":") + json_str(g.name) + ",\"windows\":" + std::to_string(g.windows) +
             ",\"open_s\":" + fmt("%.3f", g.open_seconds) + ",\"max_edge\":" + std::to_string(g.max_edge) + "}";
    }
    j += "],\"tokens\":[";
    std::vector<std::uint32_t> ti;
    for (std::uint32_t i = 0; i < e.tokens(); ++i) ti.push_back(i);
    std::sort(ti.begin(), ti.end(), [&](std::uint32_t a, std::uint32_t b) {
        const double x1 = std::abs(e.maker(a).inventory()) + static_cast<double>(e.maker(a).quotes()) * 1e-6;
        const double x2 = std::abs(e.maker(b).inventory()) + static_cast<double>(e.maker(b).quotes()) * 1e-6;
        return x1 != x2 ? x1 > x2 : a < b;
    });
    std::unordered_map<std::string, const std::string*> label;
    for (const auto& t : x.s.tokens) label[t.id] = &t.label;
    for (std::size_t k = 0; k < ti.size() && k < 25; ++k) {
        const TokenMaker& m = e.maker(ti[k]);
        const auto it = label.find(e.token_id(ti[k]));
        j += (k ? "," : "") + std::string("{\"label\":") + json_str(it == label.end() ? e.token_id(ti[k]) : *it->second) +
             ",\"best_bid\":" + std::to_string(m.book.best_bid()) + ",\"best_ask\":" + std::to_string(m.book.best_ask()) +
             ",\"bid\":" + std::to_string(m.bid_px()) + ",\"ask\":" + std::to_string(m.ask_px()) +
             ",\"inv\":" + fmt("%.0f", m.inventory()) + ",\"pnl\":" + fmt("%.2f", m.pnl()) +
             ",\"quotes\":" + std::to_string(m.quotes()) + ",\"on\":" + (m.enabled() ? "true" : "false") + "}";
    }
    j += "]}";
    return {t, j};
}

void pin(int cpu) {
    if (cpu < 0) return;
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof s, &s) != 0) std::fprintf(stderr, "cannot pin to cpu %d\n", cpu);
}

// FNV-1a over every decision, drained from the engine as it goes.
struct DecisionHash {
    std::uint64_t h = 1469598103934665603ull, n = 0;
    void drain(Engine& e) {
        for (const Decision& d : e.decisions()) {
            const std::int64_t f[5] = {d.kind, d.ns, d.token, d.bid, d.ask};
            const auto* p = reinterpret_cast<const unsigned char*>(f);
            for (std::size_t i = 0; i < sizeof f; ++i) h = (h ^ p[i]) * 1099511628211ull;
        }
        n += e.decisions().size();
        e.decisions().clear();
    }
};

void summary(Engine& e, const DecisionHash& dh, std::FILE* out) {
    std::fprintf(out, "group\twindows\topen_s\tmax_edge_bp\n");
    for (const auto& g : e.arb().stats())
        if (g.windows) std::fprintf(out, "%s\t%llu\t%.3f\t%d\n", g.name.c_str(), (unsigned long long)g.windows, g.open_seconds, g.max_edge);
    std::fprintf(stderr, "events %llu trades %llu paper_fills %llu reconnects %llu pnl_usd %.2f gross %.0f halted %d "
                 "arb_windows_closed %zu decisions %zu decision_hash %016llx\n",
                 (unsigned long long)e.events(), (unsigned long long)e.trades(), (unsigned long long)e.fills(),
                 (unsigned long long)e.reconnects(), e.pnl(), e.gross(), e.halted(), e.arb_closed().size(),
                 (std::size_t)dh.n, (unsigned long long)dh.h);
}

int replay(const Options& o) {
    const Session s = Session::load(o.replay / "session.tsv");
    Engine e(o.engine);
    s.apply(e);
    DecisionHash dh;
    const auto st = read_records(o.replay, [&](std::int64_t ns, const Event& ev) {
        e.on_event(ns, ev);
        dh.drain(e);
    });
    e.finish(INT64_MAX);
    std::fprintf(stderr, "files %zu messages %llu parse_errors %llu\n", st.files, (unsigned long long)st.messages,
                 (unsigned long long)st.parse_errors);
    summary(e, dh, stdout);
    return 0;
}

int live(const Options& o) {
    const Session s = discover(o);
    if (s.tokens.empty()) {
        std::fprintf(stderr, "no complete events found\n");
        return 1;
    }
    std::unique_ptr<Recorder> rec;
    if (!o.record.empty()) {
        fs::create_directories(o.record);
        s.save(o.record / "session.tsv");
        rec = std::make_unique<Recorder>(o.record, o.record_cap_gb);
    }
    Engine e(o.engine);
    s.apply(e);

    Doorbell bell;
    std::vector<std::unique_ptr<Feed>> feeds;
    for (const auto& t : s.tokens) {
        while (static_cast<int>(feeds.size()) <= t.conn) feeds.push_back(std::make_unique<Feed>(static_cast<int>(feeds.size()), bell));
        feeds[static_cast<std::size_t>(t.conn)]->ids.push_back(t.id);
    }
    std::vector<std::thread> threads;
    for (auto& f : feeds) threads.emplace_back([&f] { feed_loop(*f); });

    Metrics metrics;
    net::HttpServer http;
    if (!http.listen(static_cast<std::uint16_t>(o.port))) std::fprintf(stderr, "http: cannot listen on %d\n", o.port);
    else std::fprintf(stderr, "dashboard http://127.0.0.1:%u/\n", http.port());
    std::thread web([&] {
        while (!g_stop)
            http.poll_once(200, [&](std::string_view p) {
                if (p == "/metrics") return net::HttpResponse{200, "text/plain; version=0.0.4", metrics.text()};
                if (p == "/metrics.json") return net::HttpResponse{200, "application/json", metrics.json()};
                if (p == "/") return net::HttpResponse{200, "text/html; charset=utf-8", std::string(kDashboard)};
                return net::HttpResponse{404, "text/plain", "not found\n"};
            });
    });

    pin(o.cpu);
    const double tpn = tsc::ticks_per_ns();
    Latency window, total;
    Decoder dec;
    DecisionHash dh;
    const auto t0 = std::chrono::steady_clock::now();
    auto next_pub = t0;
    std::int64_t last_clock = 0;
    std::uint64_t records = 0;
    const std::string clock = control("_clock", -1);
    int idle = 0;
    // rx_tsc is 0 for the trading thread's own clock records, which are not timed.
    auto process = [&](std::int64_t ns, std::uint64_t rx_tsc, std::string_view text) {
        const std::uint64_t a = tsc::start();
        std::uint64_t in_engine = 0;
        dec.decode(text, [&](const Event& ev) {
            const std::uint64_t b = tsc::start();
            if (ev.exch_ms > 0) {
                const std::int64_t d = ns - ev.exch_ms * 1'000'000 + kExchShift;
                window.exch.record(d > 0 ? static_cast<std::uint64_t>(d) : 0);
            }
            e.on_event(ns, ev);
            in_engine += tsc::stop() - b;
        });
        const std::uint64_t z = tsc::stop();
        if (rx_tsc) {
            window.queue.record(a - rx_tsc), total.queue.record(a - rx_tsc);
            window.parse.record(z - a - in_engine), total.parse.record(z - a - in_engine);
            window.engine.record(in_engine), total.engine.record(in_engine);
            window.total.record(z - rx_tsc), total.total.record(z - rx_tsc);
        }
        dh.drain(e);
        if (rec) rec->write(ns, text);
        ++records;
    };
    while (!g_stop) {
        const std::uint32_t rung = bell.snapshot();  // taken before polling, so no wake-up is lost
        bool any = false;
        for (auto& f : feeds) {
            SpscBytes::Rec r;
            if (!f->ring.try_read(r)) continue;
            any = true;
            process(r.ns, r.tsc, r.data);
            f->ring.release();
        }
        const auto now = std::chrono::steady_clock::now();
        if (!any) {
            // Risk checks need a clock even when the market is quiet; it goes through the
            // same path so a replay sees it too.
            const std::int64_t wall = now_ns();
            if (wall - last_clock >= 100'000'000) process(wall, 0, clock), last_clock = wall;
            if (o.spin || ++idle < 256) _mm_pause();
            else bell.wait(rung, std::max<long>(1, (last_clock + 100'000'000 - wall) / 1000));
        } else {
            idle = 0;
        }
        if (now >= next_pub) {
            const double up = std::chrono::duration<double>(now - t0).count();
            auto [txt, js] = render({e, s, feeds, window, total, tpn, up, records});
            metrics.publish(std::move(txt), std::move(js));
            window.reset();
            next_pub = now + std::chrono::seconds(1);
            if (o.seconds > 0 && up >= o.seconds) g_stop = true;
        }
    }
    for (auto& t : threads) t.join();
    web.join();
    rec.reset();
    e.finish(now_ns());
    summary(e, dh, stdout);
    const double ns = 1 / tpn;
    std::fprintf(stderr, "latency ns p50/p99/p99.9: queue %.0f/%.0f/%.0f parse %.0f/%.0f/%.0f engine %.0f/%.0f/%.0f "
                 "wire_to_decision %.0f/%.0f/%.0f\n",
                 total.queue.percentile(50) * ns, total.queue.percentile(99) * ns, total.queue.percentile(99.9) * ns,
                 total.parse.percentile(50) * ns, total.parse.percentile(99) * ns, total.parse.percentile(99.9) * ns,
                 total.engine.percentile(50) * ns, total.engine.percentile(99) * ns, total.engine.percentile(99.9) * ns,
                 total.total.percentile(50) * ns, total.total.percentile(99) * ns, total.total.percentile(99.9) * ns);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        auto num = [&] { return std::atof(val().c_str()); };
        if (a == "--events") o.events = static_cast<int>(num());
        else if (a == "--max-tokens") o.max_tokens = static_cast<int>(num());
        else if (a == "--tokens-per-conn") o.tokens_per_conn = std::max(1, static_cast<int>(num()));
        else if (a == "--port") o.port = static_cast<int>(num());
        else if (a == "--record") o.record = val();
        else if (a == "--record-cap-gb") o.record_cap_gb = num();
        else if (a == "--replay") o.replay = val();
        else if (a == "--seconds") o.seconds = num();
        else if (a == "--cpu") o.cpu = static_cast<int>(num());
        else if (a == "--no-quote") o.engine.quote = false;
        else if (a == "--spin") o.spin = true;
        else if (a == "--gamma") o.engine.maker.gamma = num();
        else if (a == "--k") o.engine.maker.k = num();
        else if (a == "--size") o.engine.maker.size = num();
        else if (a == "--max-inv") o.engine.maker.max_inv = num();
        else if (a == "--horizon-s") o.engine.maker.horizon_s = num();
        else if (a == "--loss-stop") o.engine.loss_stop_usd = num();
        else if (a == "--max-gross") o.engine.max_gross_shares = num();
        else if (a == "--stale-s") o.engine.stale_s = num();
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    try {
        const int rc = o.replay.empty() ? live(o) : replay(o);
        return g_signal ? 128 + g_signal : rc;  // a stop request is not a crash to restart after
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
