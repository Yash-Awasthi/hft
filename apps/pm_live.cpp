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
#include "pm/session.hpp"

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

// ---------------------------------------------------------------- metrics

// Receive time minus the exchange's own stamp can be negative when the clocks disagree, so it
// is stored shifted by this much.
constexpr std::int64_t kExchShift = 10'000'000'000;

struct Latency {
    Histogram queue, parse, engine, total, exch;  // tsc ticks, except exch in ns plus kExchShift
    void reset() { queue.reset(), parse.reset(), engine.reset(), total.reset(), exch.reset(); }
    void merge(const Latency& o) { queue.merge(o.queue), parse.merge(o.parse), engine.merge(o.engine), total.merge(o.total); }
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

// Percentiles of one histogram in ns, taken on the trading thread so the HTTP thread never
// touches a histogram.
struct Pct {
    double p50 = 0, p99 = 0, p999 = 0, max = 0;
    std::uint64_t n = 0;
};
Pct pct(const Histogram& h, double scale, double shift = 0) {
    if (!h.count()) return {};
    static constexpr double ps[] = {50, 99, 99.9};
    std::uint64_t q[3];
    h.percentiles(ps, q);
    auto v = [&](std::uint64_t x) { return static_cast<double>(x) * scale - shift; };
    return {v(q[0]), v(q[1]), v(q[2]), v(h.max()), h.count()};
}

struct TokRow {
    std::uint32_t tok;
    std::int32_t best_bid, best_ask, bid, ask;
    double inv, pnl;
    std::uint64_t quotes;
    bool on;
};
struct GroupRow {
    std::uint32_t group;
    std::uint64_t windows;
    double open_s;
    std::int32_t max_edge;
};

// What the trading thread hands over once a second: plain numbers, copied into buffers that
// are reused, so taking it allocates nothing after the first second.
struct State {
    double uptime_s = 0, pnl = 0, gross = 0;
    std::uint64_t records = 0, events = 0, trades = 0, fills = 0, arb_open = 0, arb_windows = 0, fallbacks = 0;
    bool halted = false;
    std::vector<TokRow> toks;
    std::vector<GroupRow> groups;
    bool ready = false;
};

void capture(State& st, const Engine& e, double up, std::uint64_t records, std::uint64_t fallbacks) {
    st.uptime_s = up, st.records = records, st.fallbacks = fallbacks;
    st.events = e.events(), st.trades = e.trades(), st.fills = e.fills();
    st.pnl = e.pnl(), st.gross = e.gross(), st.halted = e.halted();
    st.arb_open = e.arb().open_windows();
    st.toks.resize(e.tokens());
    for (std::uint32_t i = 0; i < e.tokens(); ++i) {
        const TokenMaker& m = e.maker(i);
        st.toks[i] = {i, m.book.best_bid(), m.book.best_ask(), m.bid_px(), m.ask_px(), m.inventory(), m.pnl(), m.quotes(), m.enabled()};
    }
    const auto& gs = e.arb().stats();
    st.groups.resize(gs.size());
    st.arb_windows = 0;
    for (std::uint32_t g = 0; g < gs.size(); ++g) {
        st.groups[g] = {g, gs[g].windows, gs[g].open_seconds, gs[g].max_edge};
        st.arb_windows += gs[g].windows;
    }
    st.ready = true;
}

// Names fixed at startup, indexed like the engine's tokens and groups.
struct Names {
    std::vector<std::string> tokens, groups;
};

std::string json_lat(const Pct& p) {
    return "{\"p50\":" + fmt("%.0f", p.p50) + ",\"p99\":" + fmt("%.0f", p.p99) + ",\"p999\":" + fmt("%.0f", p.p999) +
           ",\"max\":" + fmt("%.0f", p.max) + ",\"n\":" + std::to_string(p.n) + "}";
}

// Runs on the HTTP thread, from a State only.
// Latency percentiles of the last full window and of the whole run.
struct Lat {
    Pct win[5], tot[4];  // queue, parse, engine, wire to decision, and (window only) exchange to receive
};

std::pair<std::string, std::string> render(const State& x, const Lat& lat, const Names& names,
                                           const std::vector<std::unique_ptr<Feed>>& feeds) {
    std::uint64_t msgs = 0, bytes = 0, recon = 0, drops = 0;
    for (const auto& f : feeds) msgs += f->msgs, bytes += f->bytes, recon += f->reconnects, drops += f->drops;

    std::string t;
    auto line = [&](const std::string& k, double v) { t += k + " " + fmt("%.6g", v) + "\n"; };
    line("hft_uptime_seconds", x.uptime_s);
    line("hft_messages_total", static_cast<double>(msgs));
    line("hft_bytes_total", static_cast<double>(bytes));
    line("hft_records_total", static_cast<double>(x.records));
    line("hft_events_total", static_cast<double>(x.events));
    line("hft_trades_total", static_cast<double>(x.trades));
    line("hft_paper_fills_total", static_cast<double>(x.fills));
    line("hft_reconnects_total", static_cast<double>(recon));
    line("hft_ring_drops_total", static_cast<double>(drops));
    line("hft_decoder_fallbacks_total", static_cast<double>(x.fallbacks));
    line("hft_pnl_usd", x.pnl);
    line("hft_gross_shares", x.gross);
    line("hft_halted", x.halted);
    line("hft_arb_open_windows", static_cast<double>(x.arb_open));
    line("hft_arb_windows_total", static_cast<double>(x.arb_windows));
    const char* stages[] = {"queue", "parse", "engine", "wire_to_decision"};
    for (int i = 0; i < 4; ++i) {
        const std::string k = std::string("hft_latency_ns{stage=\"") + stages[i] + "\",quantile=\"";
        line(k + "0.5\"}", lat.tot[i].p50), line(k + "0.99\"}", lat.tot[i].p99), line(k + "0.999\"}", lat.tot[i].p999);
    }

    std::string j = "{\"uptime_s\":" + fmt("%.1f", x.uptime_s) + ",\"messages\":" + std::to_string(msgs) +
                    ",\"bytes\":" + std::to_string(bytes) + ",\"events\":" + std::to_string(x.events) +
                    ",\"trades\":" + std::to_string(x.trades) + ",\"fills\":" + std::to_string(x.fills) +
                    ",\"reconnects\":" + std::to_string(recon) + ",\"drops\":" + std::to_string(drops) +
                    ",\"pnl\":" + fmt("%.2f", x.pnl) + ",\"gross\":" + fmt("%.0f", x.gross) +
                    ",\"halted\":" + (x.halted ? "true" : "false") + ",\"arb_open\":" + std::to_string(x.arb_open) +
                    ",\"arb_windows\":" + std::to_string(x.arb_windows);
    j += ",\"lat\":{\"queue\":" + json_lat(lat.win[0]) + ",\"parse\":" + json_lat(lat.win[1]) + ",\"engine\":" + json_lat(lat.win[2]) +
         ",\"total\":" + json_lat(lat.win[3]) + ",\"exch\":" + json_lat(lat.win[4]) + "}";
    j += ",\"conns\":[";
    const std::int64_t now = now_ns();
    for (std::size_t i = 0; i < feeds.size(); ++i) {
        const Feed& f = *feeds[i];
        const std::int64_t rx = f.last_rx_ns;
        j += (i ? "," : "") + std::string("{\"tokens\":") + std::to_string(f.ids.size()) + ",\"messages\":" +
             std::to_string(f.msgs) + ",\"reconnects\":" + std::to_string(f.reconnects) +
             ",\"age_s\":" + fmt("%.1f", rx ? static_cast<double>(now - rx) * 1e-9 : -1.0) + "}";
    }
    j += "],\"groups\":[";
    std::vector<GroupRow> gs = x.groups;
    auto gname = [&](std::uint32_t g) -> const std::string& { return names.groups[g]; };
    std::sort(gs.begin(), gs.end(), [&](const GroupRow& a, const GroupRow& b) {
        return a.windows != b.windows ? a.windows > b.windows : gname(a.group) < gname(b.group);
    });
    for (std::size_t k = 0; k < gs.size() && k < 25; ++k)
        j += (k ? "," : "") + std::string("{\"name\":") + json_str(gname(gs[k].group)) + ",\"windows\":" +
             std::to_string(gs[k].windows) + ",\"open_s\":" + fmt("%.3f", gs[k].open_s) +
             ",\"max_edge\":" + std::to_string(gs[k].max_edge) + "}";
    j += "],\"tokens\":[";
    std::vector<TokRow> ts = x.toks;
    std::sort(ts.begin(), ts.end(), [](const TokRow& a, const TokRow& b) {
        const double x1 = std::abs(a.inv) + static_cast<double>(a.quotes) * 1e-6;
        const double x2 = std::abs(b.inv) + static_cast<double>(b.quotes) * 1e-6;
        return x1 != x2 ? x1 > x2 : a.tok < b.tok;
    });
    for (std::size_t k = 0; k < ts.size() && k < 25; ++k) {
        const TokRow& r = ts[k];
        j += (k ? "," : "") + std::string("{\"label\":") + json_str(names.tokens[r.tok]) +
             ",\"best_bid\":" + std::to_string(r.best_bid) + ",\"best_ask\":" + std::to_string(r.best_ask) +
             ",\"bid\":" + std::to_string(r.bid) + ",\"ask\":" + std::to_string(r.ask) + ",\"inv\":" + fmt("%.0f", r.inv) +
             ",\"pnl\":" + fmt("%.2f", r.pnl) + ",\"quotes\":" + std::to_string(r.quotes) +
             ",\"on\":" + (r.on ? "true" : "false") + "}";
    }
    j += "]}";
    return {t, j};
}

// Hand-off between the trading thread and the HTTP thread. Once a second the trading thread
// swaps its State and its window of histograms for the ones the HTTP thread prepared, under a
// lock it only tries, so it never waits and never scans or clears a histogram itself. The HTTP
// thread folds the full window into the run totals and clears it for the next swap.
class Metrics {
   public:
    Metrics(Latency& spare, double tpn) : spare_(&spare), tpn_(tpn) {}

    void offer(State& st, Latency*& cur) {
        if (!m_.try_lock()) return;
        std::swap(shared_, st);
        if (spare_) full_ = cur, cur = spare_, spare_ = nullptr;
        m_.unlock();
    }
    void fold() {
        std::lock_guard<std::mutex> g(m_);
        if (!full_) return;
        const double ns = 1 / tpn_;
        const Histogram* w[4] = {&full_->queue, &full_->parse, &full_->engine, &full_->total};
        const Histogram* t[4] = {&total_.queue, &total_.parse, &total_.engine, &total_.total};
        total_.merge(*full_);
        for (int i = 0; i < 4; ++i) lat_.win[i] = pct(*w[i], ns), lat_.tot[i] = pct(*t[i], ns);
        lat_.win[4] = pct(full_->exch, 1, static_cast<double>(kExchShift));
        full_->reset();
        spare_ = full_, full_ = nullptr;
    }
    std::pair<std::string, std::string> page(const Names& names, const std::vector<std::unique_ptr<Feed>>& feeds) {
        std::lock_guard<std::mutex> g(m_);
        if (!shared_.ready) return {"# starting\n", "{}"};
        return render(shared_, lat_, names, feeds);
    }
    // Run totals including the window still being filled; call once the threads have stopped.
    const Latency& totals(const Latency& cur) {
        fold();
        total_.merge(cur);
        return total_;
    }

   private:
    std::mutex m_;
    State shared_;
    Lat lat_;
    Latency total_;
    Latency *spare_, *full_ = nullptr;
    double tpn_;
};

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
    Decoder dec;
    Histogram parse, engine;
    const auto st = read_lines(o.replay, [&](std::int64_t ns, std::string_view text) {
        const std::uint64_t a = tsc::start();
        std::uint64_t in_engine = 0;
        const bool ok = dec.decode(text, [&](const Event& ev) {
            const std::uint64_t b = tsc::start();
            e.on_event(ns, ev);
            in_engine += tsc::stop() - b;
        });
        parse.record(tsc::stop() - a - in_engine), engine.record(in_engine);
        dh.drain(e);
        return ok;
    });
    e.finish(INT64_MAX);
    std::fprintf(stderr, "files %zu messages %llu parse_errors %llu\n", st.files, (unsigned long long)st.messages,
                 (unsigned long long)st.parse_errors);
    const double ns = 1 / tsc::ticks_per_ns();
    std::fprintf(stderr, "per message ns p50/p99/p99.9: parse %.0f/%.0f/%.0f engine %.0f/%.0f/%.0f; tape fallbacks %llu\n",
                 parse.percentile(50) * ns, parse.percentile(99) * ns, parse.percentile(99.9) * ns,
                 engine.percentile(50) * ns, engine.percentile(99) * ns, engine.percentile(99.9) * ns,
                 (unsigned long long)dec.fallbacks());
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

    Names names;
    {
        std::unordered_map<std::string, const std::string*> label;
        for (const auto& t : s.tokens) label[t.id] = &t.label;
        for (std::uint32_t i = 0; i < e.tokens(); ++i) {
            const auto it = label.find(e.token_id(i));
            names.tokens.push_back(it == label.end() ? e.token_id(i) : *it->second);
        }
        for (const auto& g : e.arb().stats()) names.groups.push_back(g.name);
    }
    const double tpn = tsc::ticks_per_ns();
    Latency lat[2];
    Latency* window = &lat[0];
    Metrics metrics(lat[1], tpn);
    net::HttpServer http;
    if (!http.listen(static_cast<std::uint16_t>(o.port))) std::fprintf(stderr, "http: cannot listen on %d\n", o.port);
    else std::fprintf(stderr, "dashboard http://127.0.0.1:%u/\n", http.port());
    std::thread web([&] {
        while (!g_stop) {
            metrics.fold();
            http.poll_once(200, [&](std::string_view p) {
                if (p == "/metrics") return net::HttpResponse{200, "text/plain; version=0.0.4", metrics.page(names, feeds).first};
                if (p == "/metrics.json") return net::HttpResponse{200, "application/json", metrics.page(names, feeds).second};
                if (p == "/") return net::HttpResponse{200, "text/html; charset=utf-8", std::string(kDashboard)};
                return net::HttpResponse{404, "text/plain", "not found\n"};
            });
        }
    });

    pin(o.cpu);
    State state;
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
                window->exch.record(d > 0 ? static_cast<std::uint64_t>(d) : 0);
            }
            e.on_event(ns, ev);
            in_engine += tsc::stop() - b;
        });
        const std::uint64_t z = tsc::stop();
        if (rx_tsc) {
            window->queue.record(a - rx_tsc);
            window->parse.record(z - a - in_engine);
            window->engine.record(in_engine);
            window->total.record(z - rx_tsc);
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
            capture(state, e, up, records, dec.fallbacks());
            metrics.offer(state, window);
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
    const Latency& total = metrics.totals(*window);
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
    std::signal(SIGHUP, SIG_IGN);  // closing the session that started it is not a stop
    try {
        const int rc = o.replay.empty() ? live(o) : replay(o);
        return g_signal ? 128 + g_signal : rc;  // a stop request is not a crash to restart after
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
