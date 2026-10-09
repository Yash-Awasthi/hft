#include "experiment/registry.hpp"

#include <sqlite3.h>

#include <stdexcept>

namespace hft::experiment {

namespace {

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("registry: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt& text(int i, const std::string& v) {
        sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& integer(int i, std::int64_t v) {
        sqlite3_bind_int64(s, i, v);
        return *this;
    }
    std::string col(int i) const {
        const auto* p = sqlite3_column_text(s, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    }
};

}  // namespace

Registry::Registry(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        const std::string msg = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        throw std::runtime_error("registry: " + msg);
    }
    exec(R"(CREATE TABLE IF NOT EXISTS runs (
        run_id INTEGER PRIMARY KEY,
        started TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
        finished TEXT,
        name TEXT NOT NULL,
        question TEXT NOT NULL,
        config_hash TEXT NOT NULL,
        config TEXT NOT NULL,
        commit_hash TEXT NOT NULL,
        data_hash TEXT NOT NULL,
        test_access INTEGER NOT NULL,
        metrics TEXT,
        outputs TEXT))");
}

Registry::~Registry() { sqlite3_close(db_); }

void Registry::exec(const char* sql) const {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("registry: " + msg);
    }
}

std::int64_t Registry::begin(const RunConfig& c, const std::string& commit,
                             const std::string& data_hash, bool test_access) {
    Stmt st(db_,
            "INSERT INTO runs (name, question, config_hash, config, commit_hash, data_hash, "
            "test_access) VALUES (?, ?, ?, ?, ?, ?, ?)");
    st.text(1, c.name).text(2, c.question).text(3, c.hash).text(4, c.canonical).text(5, commit);
    st.text(6, data_hash).integer(7, test_access ? 1 : 0);
    if (sqlite3_step(st.s) != SQLITE_DONE)
        throw std::runtime_error(std::string("registry: ") + sqlite3_errmsg(db_));
    return sqlite3_last_insert_rowid(db_);
}

void Registry::finish(std::int64_t run, const std::string& metrics_json,
                      const std::string& outputs) {
    Stmt st(db_,
            "UPDATE runs SET finished = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), metrics = ?, "
            "outputs = ? WHERE run_id = ?");
    st.text(1, metrics_json).text(2, outputs).integer(3, run);
    if (sqlite3_step(st.s) != SQLITE_DONE || sqlite3_changes(db_) != 1)
        throw std::runtime_error("registry: cannot finish run " + std::to_string(run));
}

std::int64_t Registry::count(const std::string& question) const {
    Stmt st(db_, "SELECT COUNT(*) FROM runs WHERE question = ?");
    st.text(1, question);
    sqlite3_step(st.s);
    return sqlite3_column_int64(st.s, 0);
}

std::vector<TestAccess> Registry::test_accesses() const {
    Stmt st(db_,
            "SELECT run_id, started, question, config_hash FROM runs WHERE test_access = 1 "
            "ORDER BY run_id");
    std::vector<TestAccess> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back({sqlite3_column_int64(st.s, 0), st.col(1), st.col(2), st.col(3)});
    return out;
}

}  // namespace hft::experiment
