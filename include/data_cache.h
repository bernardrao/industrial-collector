#pragma once
/**
 * data_cache.h — 本地 SQLite 时序缓存（store-and-forward / 断点续传）
 *
 * 无论 MQTT 是否在线，每个设备每轮采集都缓存一行（batch JSON）。
 * 断点续传以 SQLite rowid 作为单调游标，跨设备不冲突。
 *
 * 表 cache(id PK AUTOINCREMENT, device_id, ts, topic, payload)
 * 表 meta(key PRIMARY KEY, value)   —— 持久化 send_cursor（普通续传断点）
 *
 * 线程安全：单 sqlite3* + 内部 mutex，N 个采集线程写 + 1 个回放线程读，全部串行化。
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;

namespace industrial {

struct CacheRow {
    int64_t     id;        // rowid，断点游标
    std::string device_id;
    std::string ts;        // ISO8601
    std::string topic;     // 原实时主题
    std::string payload;   // batch JSON
};

class DataCache {
public:
    DataCache(const std::string& db_path,
              int retention_hours,
              int64_t max_rows);
    ~DataCache();

    bool ok() const { return db_ != nullptr; }

    // 写入一行（设备一轮）。在内部缓冲区积攒，满 BATCH_FLUSH_SIZE 条即提交；
    // 另有后台线程每 BATCH_FLUSH_MS 毫秒兜底提交一次 —— 否则采集停止后
    // 缓冲区里的残留行会一直悬在内存，进程被 SIGKILL 即丢失。
    void store(const std::string& device_id, const std::string& ts,
               const std::string& topic, const std::string& payload);

    // 强制刷盘；析构时自动调用，保证关闭前数据入库。
    void flush();

    // 游标续传：返回 id > cursor 的行，最多 limit 条（按 id 升序）。
    std::vector<CacheRow> querySince(int64_t cursor, int limit);

    // 时间段续传：[start_ts, end_ts] 内、id > after_id 的行，最多 limit 条。
    std::vector<CacheRow> queryRange(const std::string& start_ts,
                                     const std::string& end_ts,
                                     int64_t after_id, int limit);

    // 普通续传断点（持久化在 meta 表）
    int64_t sendCursor();
    void    setSendCursor(int64_t cursor);

    int64_t maxId();
    int64_t rowCount();

private:
    static constexpr size_t  BATCH_FLUSH_SIZE = 16;    // 满 16 条立即提交
    static constexpr int64_t BATCH_FLUSH_MS   = 2000;  // 或距上次 >2s 提交

    struct PendRow { std::string device_id, ts, topic, payload; };

    sqlite3*           db_ = nullptr;
    mutable std::mutex mtx_;
    int                retention_hours_;
    int64_t            max_rows_;
    int64_t            inserts_since_prune_ = 0;

    std::vector<PendRow>                  pending_;
    std::chrono::steady_clock::time_point last_flush_;

    // 定时兜底刷盘线程
    std::thread             flusher_;
    std::condition_variable flush_cv_;
    std::atomic<bool>       stop_{false};

    void exec(const char* sql);
    void flusherLoop();
    void prune_locked();            // 调用方已持锁
    void flushPending_locked();     // 调用方已持锁
    int64_t sendCursor_locked();    // 调用方已持锁（prune 需要，避免自锁）
    int64_t countUnsent_locked(const std::string& where);
};

} // namespace industrial
