// test/data_cache_test.cpp
//
// 本地缓存：定时兜底刷盘、prepared statement 复用后的写入正确性、
// prune 丢弃"尚未补传"的行时必须告警（有界缓存丢数据不能静默）。
//
// 注意 prune 每 PRUNE_EVERY(200) 次插入才触发一次，故行数不会时刻等于 max_rows。
//
// 编译：
//   cd build && make data_cache_test && ./data_cache_test

#include "data_cache.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>
#include <unistd.h>

using namespace industrial;
using namespace std::chrono;
static int g_fail = 0;
#define CHECK(c,m) do{ if(c) printf("  ✓ %s\n",m); else {printf("  ✗ FAIL %s\n",m);++g_fail;} }while(0)

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp";

    auto ss   = std::make_shared<std::ostringstream>();
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*ss);
    auto lg   = std::make_shared<spdlog::logger>("t", sink);
    lg->set_level(spdlog::level::debug);
    spdlog::set_default_logger(lg);

    printf("══ data_cache ══\n");

    // ── 1. 定时兜底刷盘：只写 3 条（<16），不调用 flush()，等后台线程提交 ──────
    {
        const std::string db = dir + "/t1.db";
        ::unlink(db.c_str());
        printf("\n── 定时兜底刷盘（3 条 < BATCH_FLUSH_SIZE=16，不手动 flush）──\n");
        DataCache c(db, 24, 1000000);
        for (int i = 0; i < 3; ++i)
            c.store("dev", "2026-07-09T00:00:0" + std::to_string(i) + "Z", "t/x", "{}");

        printf("    刚写入: rowCount=%lld\n", (long long)c.rowCount());
        CHECK(c.rowCount() == 0, "尚未达到批量阈值，未落盘（符合批量语义）");

        std::this_thread::sleep_for(milliseconds(2600));   // > BATCH_FLUSH_MS(2000)
        long long n = (long long)c.rowCount();
        printf("    等 2.6s 后: rowCount=%lld\n", n);
        CHECK(n == 3, "后台线程定时提交了残留行（旧实现会一直悬在内存）");
    }

    // ── 2. 批量写入正确性（复用 prepared statement）────────────────────────────
    {
        const std::string db = dir + "/t2.db";
        ::unlink(db.c_str());
        printf("\n── 复用 prepared statement 的批量写入 ──\n");
        DataCache c(db, 24, 1000000);
        for (int i = 0; i < 40; ++i)
            c.store("dev" + std::to_string(i % 3), "2026-07-09T00:00:00Z",
                    "topic/" + std::to_string(i), "payload-" + std::to_string(i));
        c.flush();
        CHECK(c.rowCount() == 40, "40 行全部入库");

        auto rows = c.querySince(0, 100);
        CHECK(rows.size() == 40, "querySince 读回 40 行");
        bool ok = true;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].payload != "payload-" + std::to_string(i)) ok = false;
            if (rows[i].topic   != "topic/"   + std::to_string(i)) ok = false;
        }
        CHECK(ok, "每行 topic/payload 正确且顺序与写入一致（bind 未串行错位）");
    }

    // ── 3. prune 丢弃未补传的行时必须告警 ────────────────────────────────────
    {
        const std::string db = dir + "/t3.db";
        ::unlink(db.c_str());
        printf("\n── prune 丢弃未补传行 → 告警 ──\n");
        ss->str("");
        DataCache c(db, 24, /*max_rows=*/10);       // 上限 10 行
        for (int i = 0; i < 260; ++i)               // 触发 PRUNE_EVERY=200
            c.store("dev", "2026-07-09T00:00:00Z", "t/x", "p" + std::to_string(i));
        c.flush();

        long long n = (long long)c.rowCount();
        // prune 每 PRUNE_EVERY=200 次插入才跑一次，所以最后一批未 prune 的行仍在
        printf("    写 260 行，上限 10 → rowCount=%lld（prune 每 200 次插入触发）\n", n);
        CHECK(n < 260 && n <= 70, "prune 已执行，行数被 max_rows 大幅削减");

        const std::string logs = ss->str();
        bool warned = logs.find("尚未补传") != std::string::npos;
        if (warned) {
            auto b = logs.find("缓存清理");
            printf("    日志: %s\n", logs.substr(b, logs.find('\n', b) - b).c_str());
        } else {
            printf("    日志: %s\n", logs.empty() ? "(空)" : logs.c_str());
        }
        CHECK(warned, "丢弃未补传的行时给出了告警（旧实现静默丢数据）");
    }

    // ── 4. 被清理的行都已补传时，不该告警 ────────────────────────────────────
    // 关键：被删的行必须全在游标之前。所以老行用过期 ts（会被保留期清理），
    // 触发 prune 的新行用未来 ts（不会被删），且 max_rows 放大到不生效。
    {
        const std::string db = dir + "/t5.db";
        ::unlink(db.c_str());
        printf("\n── 被清理的行都已补传 → 不告警 ──\n");
        DataCache c(db, 24, 1000000);
        for (int i = 0; i < 190; ++i)
            c.store("dev", "2020-01-01T00:00:00Z", "t/x", "old");   // 早已过期
        c.flush();
        c.setSendCursor(c.maxId());                                  // 全部已补传
        ss->str("");
        for (int i = 0; i < 15; ++i)                                 // 累计 205 → 触发 prune
            c.store("dev", "2030-01-01T00:00:00Z", "t/x", "new");    // 未来，不会被删
        c.flush();

        long long n = (long long)c.rowCount();
        printf("    过期行被清理后 rowCount=%lld（应只剩 15 条新行）\n", n);
        CHECK(n == 15, "过期行已清理，新行保留");
        const std::string logs = ss->str();
        CHECK(logs.find("尚未补传") == std::string::npos,
              "被清理的行都在游标之前 → 不告警（不是数据丢失）");
    }

    printf("\n════════════════════════════════════\n");
    if (g_fail) { printf("%d 项失败 ✗\n", g_fail); return 1; }
    printf("全部通过 ✓\n");
    return 0;
}
