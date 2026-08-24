#pragma once
/**
 * web_server.h — 内置 HTTP 管理界面（多设备版）
 */
// httplib.h 的 SSL/zlib/brotli 支持通过"不定义宏"来禁用（见 CMakeLists.txt）

#include "types.h"
#include "config.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <ctime>

namespace httplib { class Server; struct Request; struct Response; }

namespace industrial {

struct LogEntry {
    std::string timestamp;
    std::string level;
    std::string message;
};

// 每个设备（或总线上的单块表）的运行统计
//
// errors 与"点质量"是两件事，必须分开计数：
//   errors     —— 采集轮抛异常（连接断了、读失败），只由 main.cpp 的 catch 块递增
//   dead_polls —— 有点但全 BAD。轮询协议下多半是掉线；CAN 被动监听下总线静默属正常
//   bad_points —— 累计质量非 GOOD 的点数，反映点级质量
// 早先把"部分点 BAD"直接记进 errors，导致一个坏寄存器就让整轮算错误，
// 而 CAN 静默总线会让错误数无限增长。
struct DeviceStats {
    uint64_t    total_polls    = 0;
    uint64_t    good_polls     = 0;   // 全部点 GOOD 的轮数
    uint64_t    dead_polls     = 0;   // 有点但全 BAD 的轮数
    uint64_t    errors         = 0;   // 采集异常的轮数
    uint64_t    total_points   = 0;
    uint64_t    bad_points     = 0;
    uint64_t    mqtt_published = 0;
    std::string protocol;
    std::string start_time;
    std::string status = "连接中";   // 连接中 / 运行中 / 错误 / 已停止
};

// ── 线程安全共享状态 ─────────────────────────────────────────────────────────
struct SharedState {
    // key = 有效设备ID（单设备: entry.id; 总线表: entry.id/meter.id）
    mutable std::mutex points_mutex;
    std::unordered_map<std::string, DataPoints> device_points;

    mutable std::mutex stats_mutex;
    std::unordered_map<std::string, DeviceStats> device_stats;

    mutable std::mutex   log_mutex;
    std::deque<LogEntry> log_ring;
    static constexpr size_t LOG_RING_MAX = 500;

    std::atomic<bool>     running{true};
    std::atomic<bool>     paused{false};

    // ── 配置热切换 ──────────────────────────────────────────────────────────
    // 「生效」编译出新版本后递增。采集线程每轮比对：变了就结束本次连接，
    // 由外层重连循环用新配置重建采集器 —— 复用已经跑熟的重连通路，而不是
    // 给七个采集器各加一套"运行中换点表"的逻辑。
    // 代价是一次亚秒级重连；换来的是改配置不必重启进程。
    std::atomic<int64_t>  reload_seq{0};
    // 当前运行的编译版本号（0 = 直接来自 config.json，未走编译快照）
    std::atomic<int64_t>  runtime_version{0};

    // SSE 推送通知：设备数据更新时 notify_all，SSE 线程等待此信号
    mutable std::mutex      push_mtx_;
    std::condition_variable push_cv_;
    std::atomic<uint64_t>   push_seq_{0};

    mutable std::mutex config_mutex;
    AppConfig          config;
    std::string        config_path;

    // ── 更新方法 ────────────────────────────────────────────────────────────
    void updatePoints(const std::string& id, const DataPoints& pts) {
        { std::lock_guard<std::mutex> lk(points_mutex); device_points[id] = pts; }
        {
            // 必须持 push_mtx_ 递增序号：SSE 线程在"持锁判定 predicate 为假、尚未阻塞"
            // 的窗口里，无锁的 notify 会丢失，该客户端要白等一个 3s 心跳才刷新。
            std::lock_guard<std::mutex> lk(push_mtx_);
            push_seq_.fetch_add(1, std::memory_order_relaxed);
        }
        push_cv_.notify_all();   // 唤醒所有 SSE 客户端线程（在锁外通知，避免立刻争锁）
    }

    // 经 spdlog sink 被所有采集线程并发调用 → 必须用 localtime_r（非共享静态 tm）
    void addLog(const std::string& lvl, const std::string& msg) {
        time_t t = time(nullptr);
        char buf[16];
        std::tm tmv{};
        localtime_r(&t, &tmv);
        strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
        { std::lock_guard<std::mutex> lk(log_mutex);
          log_ring.push_back({buf, lvl, msg});
          if (log_ring.size() > LOG_RING_MAX) log_ring.pop_front(); }
    }

    // 注意：这里【不再】递增 errors —— 部分点 BAD 不等于一轮采集失败。
    void incPoll(const std::string& id, int good, int total) {
        std::lock_guard<std::mutex> lk(stats_mutex);
        auto& s = device_stats[id];
        ++s.total_polls;
        s.total_points += (uint64_t)total;
        s.bad_points   += (uint64_t)(total - good);
        if (good == total)          ++s.good_polls;
        if (total > 0 && good == 0) ++s.dead_polls;
    }

    void incPublished(const std::string& id) {
        std::lock_guard<std::mutex> lk(stats_mutex);
        ++device_stats[id].mqtt_published;
    }

    void setStatus(const std::string& id, const std::string& status) {
        std::lock_guard<std::mutex> lk(stats_mutex);
        device_stats[id].status = status;
    }

    // 任务已不在配置里：抹掉它的统计与实时数据。
    //
    // 【必须由退出中的那个采集线程自己调用】，而不是在「生效」时统一清理 ——
    // 「生效」的那一刻各线程还没察觉世代变了，下一轮采集会把刚抹掉的行原样写
    // 回来（`device_stats[id]` 是默认构造，写回来的是一行空白幽灵）。让线程在
    // 返回前作最后一笔操作，顺序才是确定的。
    // 不保证即时：线程可能正卡在 transport->open() 或重连等待里，要等它醒来。
    // id 是任务 id；DLT 总线的分表统计以 "任务/表" 为键，一并抹掉。
    void removeDevice(const std::string& id) {
        const std::string pfx = id + "/";
        auto mine = [&](const std::string& k) { return k == id || k.rfind(pfx, 0) == 0; };
        {
            std::lock_guard<std::mutex> lk(stats_mutex);
            for (auto it = device_stats.begin(); it != device_stats.end();)
                it = mine(it->first) ? device_stats.erase(it) : std::next(it);
        }
        {
            std::lock_guard<std::mutex> lk(points_mutex);
            for (auto it = device_points.begin(); it != device_points.end();)
                it = mine(it->first) ? device_points.erase(it) : std::next(it);
        }
    }

    // DLT 总线重建后：只保留这些电表的统计/数据，其余抹掉。
    //
    // removeDevice 管不到这一层 —— 总线任务还在，消失的只是它下面的一块表，
    // 任务级的清理看不见"任务/表"这一级。而热切换前电表是不会变的，所以这个
    // 缺口是随 P2c 一起出现的：现场删掉一块表并「生效」，那块表会永远挂在实时
    // 监控上、数值定格在最后一次采集，看起来像还在跑。
    // 调用点在总线循环建好 collectors 之后 —— 那时新表集已定，且本线程正是
    // 唯一会写这些键的人，不存在"清完又被写回来"。
    void retainBusMeters(const std::string& task,
                          const std::vector<std::string>& meter_ids) {
        const std::string pfx = task + "/";
        auto gone = [&](const std::string& k) {
            if (k.rfind(pfx, 0) != 0) return false;          // 不是这条总线的键
            const std::string m = k.substr(pfx.size());
            for (const auto& id : meter_ids) if (id == m) return false;
            return true;
        };
        {
            std::lock_guard<std::mutex> lk(stats_mutex);
            for (auto it = device_stats.begin(); it != device_stats.end();)
                it = gone(it->first) ? device_stats.erase(it) : std::next(it);
        }
        {
            std::lock_guard<std::mutex> lk(points_mutex);
            for (auto it = device_points.begin(); it != device_points.end();)
                it = gone(it->first) ? device_points.erase(it) : std::next(it);
        }
    }

    // 全局聚合统计（用于 Web 顶栏显示）
    uint64_t totalPolls() const {
        std::lock_guard<std::mutex> lk(stats_mutex);
        uint64_t n = 0;
        for (auto& kv : device_stats) n += kv.second.total_polls;
        return n;
    }
    uint64_t totalErrors() const {
        std::lock_guard<std::mutex> lk(stats_mutex);
        uint64_t n = 0;
        for (auto& kv : device_stats) n += kv.second.errors;
        return n;
    }
    uint64_t totalBadPoints() const {
        std::lock_guard<std::mutex> lk(stats_mutex);
        uint64_t n = 0;
        for (auto& kv : device_stats) n += kv.second.bad_points;
        return n;
    }
    uint64_t totalPublished() const {
        std::lock_guard<std::mutex> lk(stats_mutex);
        uint64_t n = 0;
        for (auto& kv : device_stats) n += kv.second.mqtt_published;
        return n;
    }
};

// ── HTTP 服务器 ──────────────────────────────────────────────────────────────
class ConfigDb;   // 配置库（模型驱动板块用），前置声明避免头文件互相牵扯

class WebServer {
public:
    // cfgdb 可为 nullptr —— 那样只挂载实时监控与旧配置页，六大板块的
    // 模型/设备树/任务/通道接口不注册。便于在没有 config.db 时降级运行。
    // on_reload：「生效」编译成功后的热切换回调（industrial::ReloadFn，见
    // config_db.h）。此处用等价的 std::function 写出，免得 web_server.h 为一个
    // 类型别名去包含整个 config_db.h。为空表示不支持热切换。
    WebServer(SharedState& state, const WebConfig& wcfg, ConfigDb* cfgdb = nullptr,
              std::function<bool(int64_t, std::string&)> on_reload = nullptr);
    ~WebServer();

    void start();
    void stop();

    // 通道表 CRUD 完成后由前端触发（POST /api/channels/reload）。main 提供实现，
    // 会用最新的 channels 表重建通道总线（stopAll → 新 vector → startAll →
    // 重挂缓存 + 重注册下行）。返回 false 时 msg 说明原因；老通道保持工作。
    using ChannelsReloadFn = std::function<bool(std::string& msg)>;
    void setChannelsReload(ChannelsReloadFn fn) { on_ch_reload_ = std::move(fn); }

private:
    SharedState&                     state_;
    WebConfig                        wcfg_;
    ConfigDb*                        cfgdb_ = nullptr;
    std::function<bool(int64_t, std::string&)> on_reload_;
    std::function<bool(std::string&)>          on_ch_reload_;
    int                              port_;
    std::string                      bind_;
    bool                             debug_;
    std::string                      debugRoot_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread                      thread_;
    bool                             tlsActive_ = false;

    // HTTP Basic 鉴权：预先算好期望的 Authorization 头，逐字节定时安全比较，
    // 省去实现 base64 解码。空 = 未启用。
    bool        authOn_ = false;
    std::string authHeader_;

    void setupRoutes();
    void handleApiAll      (const httplib::Request&, httplib::Response&);
    void handleApiStream   (const httplib::Request&, httplib::Response&);
    void handleApiStatus   (const httplib::Request&, httplib::Response&);
    void handleApiPoints   (const httplib::Request&, httplib::Response&);
    void handleApiTreeLive (const httplib::Request&, httplib::Response&);
    void handleApiLogs     (const httplib::Request&, httplib::Response&);
    void handleApiConfig        (const httplib::Request&, httplib::Response&);
    void handleApiConfigPut     (const httplib::Request&, httplib::Response&);
    void handleApiConfigFullGet (const httplib::Request&, httplib::Response&);
    void handleApiConfigFullPost(const httplib::Request&, httplib::Response&);
    void handleApiControl       (const httplib::Request&, httplib::Response&);

    std::string        buildAllJson (int n);   // status+points+logs，供 /api/all 和 SSE 共用
    static std::string pointsToJson(const DataPoints& pts);
    static std::string logsToJson  (const std::deque<LogEntry>& logs);
};

} // namespace industrial
