// Paper execution on the prediction-market stream: makers quote through risk, the order manager
// and the simulated venue, configured by a run file (exec.cfg). No order ever leaves the machine.
//
//   pm_exec --replay DIR [--config FILE]... [--set KEY=VALUE]... [--run OUT] [--profile]
//   pm_exec --paper --run OUT --config FILE [--set KEY=VALUE]... [--seconds S] [--port P] [--cpu C]
//           [--spin] [--events N] [--max-tokens N] [--tokens-per-conn N] [--record-cap-gb G]
//           [--exec-dir DIR] [--reset-kill]
//
// Replay: the venue's reports are a function of the recording, the config and its seed, so the
// same three give the same hashes. Without --config, DIR/exec.cfg and DIR/overrides.cfg are used
// (what a paper run writes), so a paper run replays as it ran.
// Paper: live feed into a new run directory OUT: the recording, session.tsv, exec.cfg (as given),
// overrides.cfg (--set lines and the session number), summary.tsv and, on a kill, kill.log.
// Kill (D11): SIGUSR1 or a file OUT/KILL trips the switch; every order is cancelled, none placed.
// A tripped switch leaves EXEC_DIR/KILLED and later paper runs refuse to start until one is given
// --reset-kill. EXEC_DIR (default ~/data/exec) also holds the session counter (DESIGN T6).

#include "pm_app.hpp"

namespace {

void on_kill_signal(int) { g_kill = true; }

std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f) throw std::runtime_error("cannot read " + p.string());
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream f(p);
    if (!(f << text)) throw std::runtime_error("cannot write " + p.string());
}

// Next session number from EXEC_DIR/session.counter; never reused (T6).
std::uint16_t next_session(const fs::path& dir) {
    fs::create_directories(dir);
    const fs::path p = dir / "session.counter";
    long n = 0;
    if (fs::exists(p)) n = std::atol(read_file(p).c_str());
    if (n < 0 || n >= 65'535) throw std::runtime_error(p.string() + ": session numbers used up or corrupt");
    write_file(p, std::to_string(n + 1) + "\n");
    return static_cast<std::uint16_t>(n + 1);
}

int usage() {
    std::fprintf(stderr,
                 "usage: pm_exec --replay DIR [--config FILE]... [--set KEY=VALUE]... [--run OUT] [--profile]\n"
                 "       pm_exec --paper --run OUT --config FILE [--set KEY=VALUE]... [--seconds S] [--port P]\n"
                 "               [--cpu C] [--spin] [--events N] [--max-tokens N] [--tokens-per-conn N]\n"
                 "               [--record-cap-gb G] [--exec-dir DIR] [--reset-kill]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::vector<fs::path> configs;
    std::vector<std::string> sets;
    bool paper = false, reset_kill = false;
    const char* home = std::getenv("HOME");
    fs::path exec_dir = fs::path(home ? home : ".") / "data" / "exec";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(usage());
            }
            return argv[++i];
        };
        auto num = [&] { return std::atof(val().c_str()); };
        if (a == "--replay") o.replay = val();
        else if (a == "--paper") paper = true;
        else if (a == "--config") configs.push_back(val());
        else if (a == "--set") sets.push_back(val());
        else if (a == "--run") o.run = val();
        else if (a == "--exec-dir") exec_dir = val();
        else if (a == "--reset-kill") reset_kill = true;
        else if (a == "--seconds") o.seconds = num();
        else if (a == "--port") o.port = static_cast<int>(num());
        else if (a == "--cpu") o.cpu = static_cast<int>(num());
        else if (a == "--spin") o.spin = true;
        else if (a == "--events") o.events = static_cast<int>(num());
        else if (a == "--max-tokens") o.max_tokens = static_cast<int>(num());
        else if (a == "--tokens-per-conn") o.tokens_per_conn = std::max(1, static_cast<int>(num()));
        else if (a == "--record-cap-gb") o.record_cap_gb = num();
        else if (a == "--profile") o.engine.profile = true;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return usage();
        }
    }
    if (paper == !o.replay.empty() || (paper && (o.run.empty() || configs.size() != 1))) return usage();

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGHUP, SIG_IGN);
    try {
        if (configs.empty()) {
            configs.push_back(o.replay / "exec.cfg");
            if (fs::exists(o.replay / "overrides.cfg")) configs.push_back(o.replay / "overrides.cfg");
        }
        o.engine.venue = true;
        for (const fs::path& c : configs) apply_config(o.engine, read_file(c), c.string());
        for (const std::string& s : sets) apply_config(o.engine, s, "--set " + s);

        int rc;
        if (paper) {
            const fs::path latch = exec_dir / "KILLED";
            if (fs::exists(latch)) {
                if (!reset_kill) {
                    std::string run = read_file(latch);
                    if (!run.empty() && run.back() == '\n') run.pop_back();
                    std::fprintf(stderr, "kill switch tripped in %s: read its kill.log, then start with --reset-kill\n", run.c_str());
                    return 1;
                }
                fs::remove(latch);
            }
            if (fs::exists(o.run)) throw std::runtime_error(o.run.string() + " exists: a paper run needs a new directory");
            fs::create_directories(o.run);
            o.engine.session = next_session(exec_dir);
            std::string overrides;
            for (const std::string& s : sets) overrides += s + "\n";
            overrides += "session = " + std::to_string(o.engine.session) + "\n";
            write_file(o.run / "exec.cfg", read_file(configs[0]));
            write_file(o.run / "overrides.cfg", overrides);
            o.record = o.run;
            o.kill_latch = latch;
            std::signal(SIGUSR1, on_kill_signal);
            rc = live(o);
        } else {
            if (!o.run.empty()) fs::create_directories(o.run);
            rc = replay(o);
        }
        return g_signal ? 128 + g_signal : rc;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
