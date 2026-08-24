// src/data_cache.cpp — SQLite 时序缓存实现
#include "data_cache.h"
#include <sqlite3.h>
#include <spdlog/spdlog.h>
#include <ctime>

namespace industrial {

// 每多少次写入触发一次清理
static constexpr int64_t PRUNE_EVERY = 200;

DataCache::DataCache(const std::string& db_path, int retention_hours, int64_t max_rows)
    : retention_hours_(retention_hours), max_rows_(max_rows),
      last_flush_(std::chrono::steady_clock::now())
{
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        spdlog::error("缓存数据库打开失败 {}: {}", db_path,
                      db_ ? sqlite3_errmsg(db_) : "?");
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return;
    }
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("CREATE TABLE IF NOT EXISTS cache("
         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "device_id TEXT, ts TEXT, topic TEXT, payload TEXT);");
    exec("CREATE INDEX IF NOT EXISTS idx_cache_ts ON cache(ts);");
    exec("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);");
    spdlog::info("本地缓存就绪: {} (保留 {}h, 上限 {} 行)",
                 db_path, retention_hours_, (long long)max_rows_);

    flusher_ = std::thread(&DataCache::flusherLoop, this);
}

DataCache::~DataCache() {
    stop_ = true;
    flush_cv_.notify_all();
    if (flusher_.joinable()) flusher_.join();
    if (db_) {
        flush();           // 确保缓冲区数据在关闭前全部入库
        sqlite3_close(db_);
    }
}

// 定时兜底：采集停止后，缓冲区里不足 BATCH_FLUSH_SIZE 条的残留行也要落盘。
// 仅靠 store() 里的时间判断是"懒提交"—— 没有下一次 store 就永远不提交。
void DataCache::flusherLoop() {
    std::mutex cv_m;
    while (!stop_.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lk(cv_m);
            flush_cv_.wait_for(lk, std::chrono::milliseconds(BATCH_FLUSH_MS / 4),
                               [this]{ return stop_.load(std::memory_order_relaxed); });
        }
        if (stop_.load(std::memory_order_relaxed)) break;

        std::lock_guard<std::mutex> lk(mtx_);
        if (pending_.empty()) continue;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - last_flush_).count();
        if (elapsed >= BATCH_FLUSH_MS) flushPending_locked();
    }
}

void DataCache::exec(const char* sql) {
    if (!db_) return;
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        spdlog::warn("缓存 SQL 失败: {} ({})", err ? err : "?", sql);
        sqlite3_free(err);
    }
}

void DataCache::store(const std::string& device_id, const std::string& ts,
                      const std::string& topic, const std::string& payload) {
    if (!db_) return;
    std::lock_guard<std::mutex> lk(mtx_);
    pending_.push_back({device_id, ts, topic, payload});

    auto now     = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_flush_).count();
    if (pending_.size() >= BATCH_FLUSH_SIZE || elapsed >= BATCH_FLUSH_MS) {
        flushPending_locked();
    }
}

void DataCache::flush() {
    if (!db_) return;
    std::lock_guard<std::mutex> lk(mtx_);
    flushPending_locked();
}

void DataCache::flushPending_locked() {
    if (pending_.empty()) return;
    static const char* SQL =
        "INSERT INTO cache(device_id,ts,topic,payload) VALUES(?,?,?,?);";

    // 整批复用一个 prepared statement（reset 而非 finalize），省掉每行一次 SQL 编译
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, SQL, -1, &st, nullptr) != SQLITE_OK) {
        spdlog::warn("缓存 prepare 失败，丢弃 {} 行: {}", pending_.size(), sqlite3_errmsg(db_));
        pending_.clear();
        last_flush_ = std::chrono::steady_clock::now();
        return;
    }

    exec("BEGIN;");
    for (auto& row : pending_) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, row.device_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, row.ts.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, row.topic.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, row.payload.c_str(),   -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE)
            spdlog::warn("缓存写入失败: {}", sqlite3_errmsg(db_));
        ++inserts_since_prune_;
    }
    sqlite3_finalize(st);
    exec("COMMIT;");

    // prune 放在事务之外：DELETE 与本批 INSERT 无需原子
    if (inserts_since_prune_ >= PRUNE_EVERY) {
        inserts_since_prune_ = 0;
        prune_locked();
    }
    pending_.clear();
    last_flush_ = std::chrono::steady_clock::now();
}

// 统计满足 where 的行里有多少尚未补传（id > send_cursor）
int64_t DataCache::countUnsent_locked(const std::string& where) {
    const int64_t cursor = sendCursor_locked();
    std::string sql = "SELECT COUNT(*) FROM cache WHERE (" + where +
                      ") AND id > " + std::to_string(cursor) + ";";
    sqlite3_stmt* st = nullptr;
    int64_t v = 0;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

void DataCache::prune_locked() {
    if (!db_) return;

    // 1) 按保留时长删除：ts < now - retention（ISO 字符串可直接比较）
    time_t cutoff = time(nullptr) - (time_t)retention_hours_ * 3600;
    char buf[32];
    std::tm tmv{};   // gmtime_r：本函数在采集线程上跑，与 nowIso() 共享静态 tm 会竞争
    gmtime_r(&cutoff, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    const std::string byTime = "ts < '" + std::string(buf) + "'";

    // 有界缓存必然要丢老数据，但丢的若是【还没补传】的行，就是静默数据丢失 —— 必须说出来
    if (int64_t n = countUnsent_locked(byTime))
        spdlog::warn("缓存清理：{} 行超过保留期 {}h 但尚未补传，将被丢弃"
                     "（调大 cache.retention_hours 或尽快触发续传）", n, retention_hours_);
    exec(("DELETE FROM cache WHERE " + byTime + ";").c_str());

    // 2) 按行数上限删除最旧
    if (max_rows_ > 0) {
        const std::string byRows =
            "id <= (SELECT MAX(id) FROM cache) - " + std::to_string(max_rows_);
        if (int64_t n = countUnsent_locked(byRows))
            spdlog::warn("缓存清理：{} 行超出 cache.max_rows={} 上限但尚未补传，将被丢弃",
                         n, (long long)max_rows_);
        exec(("DELETE FROM cache WHERE " + byRows + ";").c_str());
    }
}

static std::vector<CacheRow> readRows(sqlite3_stmt* st) {
    std::vector<CacheRow> out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        CacheRow r;
        r.id        = sqlite3_column_int64(st, 0);
        auto txt = [&](int i){ const unsigned char* p = sqlite3_column_text(st,i);
                               return p ? std::string((const char*)p) : std::string(); };
        r.device_id = txt(1);
        r.ts        = txt(2);
        r.topic     = txt(3);
        r.payload   = txt(4);
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<CacheRow> DataCache::querySince(int64_t cursor, int limit) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return {};
    static const char* SQL =
        "SELECT id,device_id,ts,topic,payload FROM cache "
        "WHERE id>? ORDER BY id ASC LIMIT ?;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, SQL, -1, &st, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_int64(st, 1, cursor);
    sqlite3_bind_int(st, 2, limit);
    auto rows = readRows(st);
    sqlite3_finalize(st);
    return rows;
}

std::vector<CacheRow> DataCache::queryRange(const std::string& start_ts,
                                            const std::string& end_ts,
                                            int64_t after_id, int limit) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return {};
    static const char* SQL =
        "SELECT id,device_id,ts,topic,payload FROM cache "
        "WHERE ts>=? AND ts<=? AND id>? ORDER BY id ASC LIMIT ?;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, SQL, -1, &st, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_text(st, 1, start_ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, end_ts.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, after_id);
    sqlite3_bind_int(st, 4, limit);
    auto rows = readRows(st);
    sqlite3_finalize(st);
    return rows;
}

int64_t DataCache::sendCursor_locked() {
    if (!db_) return 0;
    sqlite3_stmt* st = nullptr;
    int64_t v = 0;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key='send_cursor';",
                           -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

int64_t DataCache::sendCursor() {
    std::lock_guard<std::mutex> lk(mtx_);
    return sendCursor_locked();
}

void DataCache::setSendCursor(int64_t cursor) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return;
    std::string sql =
        "INSERT INTO meta(key,value) VALUES('send_cursor','" +
        std::to_string(cursor) +
        "') ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    exec(sql.c_str());
}

int64_t DataCache::maxId() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return 0;
    sqlite3_stmt* st = nullptr;
    int64_t v = 0;
    if (sqlite3_prepare_v2(db_, "SELECT IFNULL(MAX(id),0) FROM cache;",
                           -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

int64_t DataCache::rowCount() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return 0;
    sqlite3_stmt* st = nullptr;
    int64_t v = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM cache;",
                           -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

} // namespace industrial
