// src/main.cpp — 多设备混合协议主入口
#include "config.h"
#include "mqtt_publisher.h"
#include "modbus_collector.h"
#include "iec104_collector.h"
#include "iec61850_collector.h"
#include "opcua_collector.h"
#include "dlt645_collector.h"
#include "dlt698_collector.h"
#include "can_collector.h"
#include "web_server.h"
#include "config_db.h"
#include "channel.h"

#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

// ── SharedState spdlog sink ───────────────────────────────────────────────────
template<typename Mutex>
class StateSink : public spdlog::sinks::base_sink<Mutex> {
public:
    explicit StateSink(industrial::SharedState& s) : s_(s) {}
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        std::string lv = "INFO";
        if (msg.level == spdlog::level::warn)    lv = "WARN";
        else if (msg.level >= spdlog::level::err) lv = "ERROR";
        s_.addLog(lv, std::string(msg.payload.begin(), msg.payload.end()));
    }
    void flush_() override {}
private:
    industrial::SharedState& s_;
};
using StateSinkMt = StateSink<std::mutex>;

static industrial::SharedState* g_state = nullptr;
static void onSignal(int) {
    spdlog::warn("收到停止信号");
    if (g_state) g_state->running = false;
}

static void initLogger(const industrial::LogConfig& lc,
                        industrial::SharedState& state) {
    using namespace spdlog;
    auto lvl = level::info;
    if      (lc.level=="debug") lvl=level::debug;
    else if (lc.level=="trace") lvl=level::trace;
    else if (lc.level=="warn")  lvl=level::warn;
    else if (lc.level=="error") lvl=level::err;

    auto con  = std::make_shared<sinks::stdout_color_sink_mt>();
    auto file = std::make_shared<sinks::rotating_file_sink_mt>(
                    lc.file, lc.max_bytes, lc.backup_count);
    auto web  = std::make_shared<StateSinkMt>(state);

    auto col_logger = std::make_shared<spdlog::logger>("col",
                          sinks_init_list{con, file, web});
    col_logger->set_level(lvl);
    col_logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    set_default_logger(col_logger);
}

// 轮询型协议：连续多少轮"有点但全 BAD"判定为掉线，触发外层重连
static constexpr int MAX_DEAD_CYCLES = 3;

// 被动监听型采集器（CAN）：总线静默 ≠ 链路故障 —— 没有请求可发，全 BAD 只说明
// 此刻没有目标帧在广播，重开 socket 也变不出数据，反而清空帧缓存并留下盲窗。
// 故这类采集器改用 linkOk() 判定：只有 socket 报错（接口 down / 被删除）才重连。
template<typename C> struct IsPassiveCollector : std::false_type {};
template<> struct IsPassiveCollector<industrial::CanCollector> : std::true_type {};

// 可中断睡眠：按 200ms 分片，running 置 false 时立即返回（SIGINT 不卡顿）
static void interruptibleSleep(industrial::SharedState& state, double seconds) {
    auto until = std::chrono::steady_clock::now()
               + std::chrono::milliseconds((int)(seconds * 1000));
    while (state.running && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// ── 单设备采集主循环（Modbus/IEC104/IEC61850/OPC UA/CAN）────────────────────
// 返回 = 该次连接结束（正常停止或掉线），由外层 runDeviceForever 决定是否重连。
//
// seq0 = 【取出本次 entry 时】的配置世代，由 runDeviceForever 与 entry 成对读出。
// 不在这里现读：连接建立（TCP 握手、串口打开）要花上几秒，那段时间里到来的
// 「生效」会被"进入循环才取基准"的写法吞掉 —— 该设备将一直用旧点表跑到下次
// 「生效」为止，而且手点一次几乎测不出来。所以基准必须和 entry 同一时刻取。
template<typename Collector>
static void runSingleLoop(const std::string& eid,
                           Collector& col,
                           industrial::ChannelBus& bus,
                           industrial::SharedState& state,
                           int64_t seq0)
{
    state.setStatus(eid, "运行中");
    spdlog::info("[{}] 采集循环启动", eid);

    [[maybe_unused]] int dead = 0;
    while (state.running) {
        if (state.reload_seq.load(std::memory_order_acquire) != seq0) {
            spdlog::info("[{}] 配置已更新，重建采集器", eid);
            return;
        }
        if (state.paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        auto t0 = std::chrono::steady_clock::now();

        double interval;
        {
            // 采集端只需要 interval —— topic/qos/retain 现在归通道管
            // （见 channel.h：那些参数是 MQTT 的概念，不属于采集器）。
            std::lock_guard<std::mutex> lk(state.config_mutex);
            interval = 1.0;
            for (auto& e : state.config.devices)
                if (e.id == eid) { interval = e.pollInterval(); break; }
        }

        int good = 0, total = 0;
        try {
            auto pts  = col.readAll();
            total = (int)pts.size();
            for (auto& p : pts)
                if (p.quality == industrial::Quality::GOOD) ++good;
            state.updatePoints(eid, pts);
            state.incPoll(eid, good, total);
            bus.publishRound(eid, pts);
            state.incPublished(eid);
        } catch (const std::exception& e) {
            spdlog::error("[{}] 采集异常: {}", eid, e.what());
            std::lock_guard<std::mutex> lk(state.stats_mutex);
            ++state.device_stats[eid].errors;
        }

        // 掉线检测
        if constexpr (IsPassiveCollector<Collector>::value) {
            // 被动监听：只认 socket 故障；总线静默保持运行（点为 BAD）
            if (!col.linkOk()) {
                spdlog::warn("[{}] 链路故障，触发重连", eid);
                return;
            }
        } else {
            // 轮询：连续 N 轮"有点但全 BAD" → 返回触发重连
            if (total > 0 && good == 0) {
                if (++dead >= MAX_DEAD_CYCLES) {
                    spdlog::warn("[{}] 连续 {} 轮无有效数据，触发重连", eid, dead);
                    return;
                }
            } else {
                dead = 0;
            }
        }

        auto elapsed  = std::chrono::steady_clock::now() - t0;
        auto sleep_ms = std::chrono::milliseconds((int)(interval*1000)) - elapsed;
        if (sleep_ms > std::chrono::milliseconds(0))
            std::this_thread::sleep_for(sleep_ms);
    }
}

// ── DLT645 总线循环（多表共享同一 transport）────────────────────────────────
static void runBusDlt645(const industrial::DeviceEntry& entry,
                          industrial::ChannelBus& chbus,
                          industrial::SharedState& state,
                          int64_t seq0)
{
    auto& bus = entry.dlt645;
    if (bus.meters.empty()) {
        spdlog::warn("[{}] DLT645 buses 无表配置，跳过", entry.id);
        return;
    }

    std::shared_ptr<industrial::ITransport> transport;
    if (bus.connection_type == "tcp")
        transport = std::make_shared<industrial::TcpTransport>(bus.host, bus.tcp_port);
    else
        transport = std::make_shared<industrial::SerialTransport>(bus.serial_port, bus.baud_rate);

    transport->open();   // 失败抛异常，由外层 runDeviceForever 重连

    // 每块表一个 collector，共享同一 transport
    std::vector<industrial::Dlt645Collector> collectors;
    for (auto& m : bus.meters) {
        collectors.emplace_back(transport, m, bus.timeout_ms);
        std::string eid = entry.id + "/" + m.id;
        std::lock_guard<std::mutex> lk(state.stats_mutex);
        state.device_stats[eid].protocol = "DLT645";
        state.device_stats[eid].status   = "运行中";
    }
    spdlog::info("[{}] DLT645 总线就绪，共 {} 块表", entry.id, collectors.size());
    {   // 「生效」删掉的电表要连它的统计一起摘掉，否则永远挂在实时监控上
        std::vector<std::string> ids;
        for (const auto& m : bus.meters) ids.push_back(m.id);
        state.retainBusMeters(entry.id, ids);
    }

    int dead = 0;
    while (state.running) {
        if (state.reload_seq.load(std::memory_order_acquire) != seq0) {
            spdlog::info("[{}] 配置已更新，重建总线", entry.id);
            return;
        }
        if (state.paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        auto t0 = std::chrono::steady_clock::now();

        double interval;
        {
            std::lock_guard<std::mutex> lk(state.config_mutex);
            interval = entry.dlt645.poll_interval;
            for (auto& e : state.config.devices)
                if (e.id == entry.id) { interval = e.dlt645.poll_interval; break; }
        }

        int round_good = 0, round_total = 0;
        for (auto& col : collectors) {
            const std::string eid = entry.id + "/" + col.meterId();
            try {
                auto pts  = col.readAll();
                int  good = 0;
                for (auto& p : pts)
                    if (p.quality == industrial::Quality::GOOD) ++good;
                round_good += good; round_total += (int)pts.size();
                state.updatePoints(eid, pts);
                state.incPoll(eid, good, (int)pts.size());
                chbus.publishRound(eid, pts);
                state.incPublished(eid);
            } catch (const std::exception& e) {
                spdlog::error("[{}] 采集异常: {}", eid, e.what());
                std::lock_guard<std::mutex> lk(state.stats_mutex);
                ++state.device_stats[eid].errors;
            }
        }

        // 掉线检测：连续 N 轮全 BAD → 返回触发重连
        if (round_total > 0 && round_good == 0) {
            if (++dead >= MAX_DEAD_CYCLES) {
                spdlog::warn("[{}] DLT645 连续 {} 轮无有效数据，触发重连", entry.id, dead);
                return;
            }
        } else {
            dead = 0;
        }

        auto elapsed  = std::chrono::steady_clock::now() - t0;
        auto sleep_ms = std::chrono::milliseconds((int)(interval*1000)) - elapsed;
        if (sleep_ms > std::chrono::milliseconds(0))
            std::this_thread::sleep_for(sleep_ms);
    }
    transport->close();
}

// ── DLT698 总线循环 ──────────────────────────────────────────────────────────
static void runBusDlt698(const industrial::DeviceEntry& entry,
                          industrial::ChannelBus& chbus,
                          industrial::SharedState& state,
                          int64_t seq0)
{
    auto& bus = entry.dlt698;
    if (bus.meters.empty()) { spdlog::warn("[{}] DLT698 无表配置", entry.id); return; }

    std::shared_ptr<industrial::ITransport> transport;
    if (bus.connection_type == "tcp")
        transport = std::make_shared<industrial::TcpTransport>(bus.host, bus.tcp_port);
    else
        transport = std::make_shared<industrial::SerialTransport>(bus.serial_port, bus.baud_rate);

    transport->open();   // 失败抛异常，由外层 runDeviceForever 重连

    std::vector<industrial::Dlt698Collector> collectors;
    for (auto& m : bus.meters) {
        collectors.emplace_back(transport, m, bus.timeout_ms);
        std::string eid = entry.id + "/" + m.id;
        std::lock_guard<std::mutex> lk(state.stats_mutex);
        state.device_stats[eid].protocol = "DLT698";
        state.device_stats[eid].status   = "运行中";
    }
    spdlog::info("[{}] DLT698 总线就绪，共 {} 块表", entry.id, collectors.size());
    {   // 「生效」删掉的电表要连它的统计一起摘掉，否则永远挂在实时监控上
        std::vector<std::string> ids;
        for (const auto& m : bus.meters) ids.push_back(m.id);
        state.retainBusMeters(entry.id, ids);
    }

    int dead = 0;
    while (state.running) {
        if (state.reload_seq.load(std::memory_order_acquire) != seq0) {
            spdlog::info("[{}] 配置已更新，重建总线", entry.id);
            return;
        }
        if (state.paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        auto t0 = std::chrono::steady_clock::now();

        double interval;
        {
            std::lock_guard<std::mutex> lk(state.config_mutex);
            interval = entry.dlt698.poll_interval;
            for (auto& e : state.config.devices)
                if (e.id == entry.id) { interval = e.dlt698.poll_interval; break; }
        }

        int round_good = 0, round_total = 0;
        for (auto& col : collectors) {
            const std::string eid = entry.id + "/" + col.meterId();
            try {
                auto pts  = col.readAll();
                int  good = 0;
                for (auto& p : pts)
                    if (p.quality == industrial::Quality::GOOD) ++good;
                round_good += good; round_total += (int)pts.size();
                state.updatePoints(eid, pts);
                state.incPoll(eid, good, (int)pts.size());
                chbus.publishRound(eid, pts);
                state.incPublished(eid);
            } catch (const std::exception& e) {
                spdlog::error("[{}] 采集异常: {}", eid, e.what());
                std::lock_guard<std::mutex> lk(state.stats_mutex);
                ++state.device_stats[eid].errors;
            }
        }

        if (round_total > 0 && round_good == 0) {
            if (++dead >= MAX_DEAD_CYCLES) {
                spdlog::warn("[{}] DLT698 连续 {} 轮无有效数据，触发重连", entry.id, dead);
                return;
            }
        } else {
            dead = 0;
        }

        auto elapsed  = std::chrono::steady_clock::now() - t0;
        auto sleep_ms = std::chrono::milliseconds((int)(interval*1000)) - elapsed;
        if (sleep_ms > std::chrono::milliseconds(0))
            std::this_thread::sleep_for(sleep_ms);
    }
    transport->close();
}

// ── 单设备入口（按协议分派）─────────────────────────────────────────────────
static void runDevice(const industrial::DeviceEntry& entry,
                       industrial::ChannelBus& bus,
                       industrial::SharedState& state,
                       int64_t seq0)
{
    switch (entry.protocol) {
        case industrial::Protocol::MODBUS: {
            industrial::ModbusCollector col(entry.modbus);
            col.connect();
            runSingleLoop(entry.id, col, bus, state, seq0);
            col.disconnect();
            break;
        }
        case industrial::Protocol::IEC104: {
            industrial::IEC104Collector col(entry.iec104);
            col.connect();
            runSingleLoop(entry.id, col, bus, state, seq0);
            col.disconnect();
            break;
        }
        case industrial::Protocol::IEC61850: {
            industrial::IEC61850Collector col(entry.iec61850);
            col.connect();
            runSingleLoop(entry.id, col, bus, state, seq0);
            col.disconnect();
            break;
        }
        case industrial::Protocol::OPCUA: {
            industrial::OpcUaCollector col(entry.opcua);
            col.connect();
            runSingleLoop(entry.id, col, bus, state, seq0);
            col.disconnect();
            break;
        }
        case industrial::Protocol::CAN: {
            industrial::CanCollector col(entry.can);
            col.connect();
            runSingleLoop(entry.id, col, bus, state, seq0);
            col.disconnect();
            break;
        }
        case industrial::Protocol::DLT645:
            runBusDlt645(entry, bus, state, seq0);
            break;
        case industrial::Protocol::DLT698:
            runBusDlt698(entry, bus, state, seq0);
            break;
    }
}

// ── 单设备无限重连包装：连接失败/掉线后按配置间隔重试，直到 running=false ──────
// 所有协议统一经此入口，重连逻辑只此一处。
// 按【设备 id】而非引用取配置：「生效」会整体替换 state.config.devices，
// 抱着启动时那份引用的话，热切换后重建出来的还是旧配置（更糟的是那份引用
// 可能已经失效）。每轮从共享配置里现取一份副本，是热切换成立的前提。
static void runDeviceForever(const std::string& devId,
                              industrial::ChannelBus& bus,
                              industrial::SharedState& state)
{
    while (state.running) {
        // 取当前配置的快照副本；找不到 = 该任务已从配置中移除，线程退出。
        // entry 与 seq0 必须在同一把锁里取 —— 写侧也是持 config_mutex 换表后
        // 才递增 reload_seq，两边配对才能保证"拿到旧表却记下新世代"不会发生。
        industrial::DeviceEntry entry;
        int64_t seq0 = 0;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(state.config_mutex);
            seq0 = state.reload_seq.load(std::memory_order_acquire);
            for (const auto& e : state.config.devices)
                if (e.id == devId) { entry = e; found = true; break; }
        }
        // 移除/停用都从实时监控里摘掉：编译快照里根本没有停用的任务，留一行
        // "已停用"反而和「生效」时的清理结果自相矛盾。
        if (!found) {
            spdlog::info("[{}] 已从配置中移除，线程退出", devId);
            state.removeDevice(devId);
            return;
        }
        if (!entry.enabled) {
            spdlog::info("[{}] 已被停用，线程退出", devId);
            state.removeDevice(devId);
            return;
        }

        const int interval = entry.reconnect_interval_sec > 0
                           ? entry.reconnect_interval_sec : 5;
        try {
            runDevice(entry, bus, state, seq0);  // 正常返回 = 停止、掉线或配置更新
        } catch (const std::exception& e) {
            spdlog::error("[{}] 连接/采集失败: {}", devId, e.what());
        }
        if (!state.running) break;

        // 因「生效」而返回的，立刻用新配置重建 —— 重连间隔是给掉线现场留的
        // 退避时间，配置更新时链路本来就是好的，没有理由让用户干等几秒。
        if (state.reload_seq.load(std::memory_order_acquire) != seq0) continue;

        state.setStatus(devId, "重连中");
        spdlog::warn("[{}] {} 秒后重连…", devId, interval);
        interruptibleSleep(state, interval);
    }
}

// ── DeviceStats 初始化 ───────────────────────────────────────────────────────
// 启动时与每次「生效」热切换后都要跑：新任务需要 protocol/start_time，否则
// 实时监控里那一行是空白的协议名。已有条目只补协议、不覆盖计数与启动时间 ——
// 换点表不该让一台连了三天的设备看起来像刚上线。
static void initDeviceStats(const std::vector<industrial::DeviceEntry>& devices,
                             industrial::SharedState& state) {
    time_t t = time(nullptr); char buf[24];
    std::tm tmv{};
    gmtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);

    std::lock_guard<std::mutex> lk(state.stats_mutex);
    auto touch = [&](const std::string& eid, const std::string& proto) {
        auto& s = state.device_stats[eid];
        s.protocol = proto;
        if (s.start_time.empty()) s.start_time = buf;
    };
    for (const auto& e : devices) {
        if (!e.enabled) continue;
        // DLT 总线：每块表有独立 stats；其他：entry 本身
        if (e.protocol == industrial::Protocol::DLT645) {
            for (const auto& m : e.dlt645.meters) touch(e.id + "/" + m.id, "DLT645");
        } else if (e.protocol == industrial::Protocol::DLT698) {
            for (const auto& m : e.dlt698.meters) touch(e.id + "/" + m.id, "DLT698");
        } else {
            // CAN FD 与经典 CAN 共用 Protocol::CAN，仅在展示上区分
            touch(e.id, (e.protocol == industrial::Protocol::CAN && e.can.fd)
                            ? "CANFD" : industrial::protocolStr(e.protocol));
        }
    }
}

// ── 采集线程池 ───────────────────────────────────────────────────────────────
// 「生效」可能新增任务，所以线程不能只在启动时建一批。live 挡住同一任务被重复
// 拉起；线程退出时自己把 id 摘掉，任务被删又加回来时还能再启。
// 已结束的 std::thread 对象不回收 —— 「生效」是人手点的，攒下几十个已完成的
// 线程句柄是有界的小开销，不值得为此引入一套完成标志的生命周期管理。
struct TaskThreads {
    std::mutex               mtx;
    std::vector<std::thread> threads;
    std::set<std::string>    live;
};

static void spawnTask(TaskThreads& tt, const std::string& devId,
                       industrial::ChannelBus& bus,
                       industrial::SharedState& state) {
    std::lock_guard<std::mutex> lk(tt.mtx);
    if (tt.live.count(devId)) return;
    tt.live.insert(devId);
    tt.threads.emplace_back([&tt, devId, &bus, &state]() {
        spdlog::info("[{}] 线程启动", devId);
        runDeviceForever(devId, bus, state);   // 内含无限重连
        { std::lock_guard<std::mutex> lk2(tt.mtx); tt.live.erase(devId); }
        spdlog::info("[{}] 线程退出", devId);
    });
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::string cfgPath = "config/config.json";
    int  webPort   = -1;
    bool debugWeb  = false;
    bool fromDb    = false;   // 采集源改用编译快照（默认仍读 config.json）

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--web-port" && i+1 < argc) webPort = std::stoi(argv[++i]);
        else if (a == "--debug") debugWeb = true;
        else if (a == "--from-db") fromDb = true;
        else if (a == "--help") {
            std::cout
                << "用法: industrial_collector [config.json] [--web-port PORT] [--debug] [--from-db]\n"
                << "  config.json 中 devices[] 支持混合协议\n"
                << "  --debug    前端从磁盘 web/ 实时伺服（改完刷新即可，免重编）\n"
                << "  --from-db  采集源改用 config.db 的编译快照（设备规格/设备树驱动）；\n"
                << "             不加则仍按 config.json 的 devices[] 采集。\n"
                << "             mqtt/web/logging 始终来自 config.json，不受影响。\n";
            return 0;
        } else { cfgPath = a; }
    }

    industrial::AppConfig cfg;
    try { cfg = industrial::loadConfig(cfgPath); }
    catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n"; return 1;
    }
    if (webPort > 0) cfg.web.port = webPort;
    if (debugWeb)    cfg.web.debug = true;   // 命令行 --debug 覆盖配置

    industrial::SharedState state;
    state.config      = cfg;      // 采集源确定后会再同步一次（见下）
    state.config_path = cfgPath;

    g_state = &state;
    initLogger(cfg.logging, state);
    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    spdlog::info("╔════════════════════════════════════════════════════╗");
    spdlog::info("║   工业数据采集程序 v1.6.0  (多设备混合协议)          ║");
    spdlog::info("║   Modbus / IEC104 / IEC61850 / OPC UA              ║");
    spdlog::info("║   DLT645 / DLT698  (串口总线 & TCP 网关)            ║");
    spdlog::info("║   CAN / CAN FD     (SocketCAN 被动监听)             ║");
    spdlog::info("╚════════════════════════════════════════════════════╝");

    // ── 配置库（模型驱动板块）──────────────────────────────────────────────
    // 与 cache.db 分开：配置低频写、跟随部署走；缓存高频写、可随时删除重建。
    //
    // 编辑设备树不会影响正在跑的现场采集：采集用的是「生效」编译出的快照，
    // 编辑只落在 config.db 上，点「生效」才换。
    const std::string cfgDbPath = "config.db";
    auto cfgdb = std::make_unique<industrial::ConfigDb>(cfgDbPath);
    if (cfgdb->ok()) {
        // 空库 = 首次运行，把现有 config.json 迁进来作为起点
        if (cfgdb->countRows("tasks") == 0 && cfgdb->countRows("tree_nodes") == 0) {
            industrial::MigrateStats mst;
            std::string merr;
            if (industrial::migrateJsonToDb(
                    industrial::appConfigToJsonString(cfg), *cfgdb, mst, merr)) {
                spdlog::info("首次运行：已从 {} 迁入配置库（任务 {} / 节点 {} / 点位 {}）",
                             cfgPath, mst.tasks, mst.nodes, mst.points);
            } else {
                spdlog::warn("配置迁入失败（不影响采集，Web 模型页将为空）: {}", merr);
            }
        }
    } else {
        spdlog::warn("配置库不可用，Web 的模型/设备树板块将不可用");
        cfgdb.reset();
    }

    // ── 采集源：config.json 的 devices[] 还是编译快照？──────────────────────
    //
    // 默认仍用 config.json，加 --from-db 才改用编译快照。这不是过渡期的犹豫，
    // 而是刻意的：快照驱动是全新路径，现场升级时应能一条命令切回旧行为。
    // 切换只替换 cfg.devices —— mqtt / web / logging 仍来自 config.json
    // （系统设置本就不入库，见 config_migrate.cpp 的职责划分）。
    if (fromDb) {
        if (!cfgdb) {
            spdlog::error("--from-db 需要可用的配置库，但 config.db 打不开");
            return 1;
        }
        const int64_t ver = cfgdb->latestVersion();
        if (ver <= 0) {
            spdlog::error("--from-db：配置库里还没有编译版本，请先在 Web 上执行「生效」");
            return 1;
        }
        std::string rjson, rerr;
        industrial::RuntimeStats rst;
        if (!industrial::buildRuntimeJson(*cfgdb, ver, rjson, rst, rerr)) {
            spdlog::error("从编译快照构建运行配置失败: {}", rerr);
            return 1;
        }
        try {
            auto rc = industrial::appConfigFromJsonString(rjson);
            cfg.devices = std::move(rc.devices);
        } catch (const std::exception& e) {
            spdlog::error("编译快照还原出的配置无法解析: {}", e.what());
            return 1;
        }
        state.runtime_version = ver;
        spdlog::info("采集源：编译快照 v{} —— 任务 {} / 测点 {}（跳过禁用 {}、空任务 {}）",
                     ver, rst.tasks, rst.points, rst.disabled_tasks, rst.empty_tasks);
    } else {
        spdlog::info("采集源：{}（如需按设备规格采集，加 --from-db）", cfgPath);
    }

    if (cfg.devices.empty()) {
        spdlog::error("没有任何设备可采集：{} 的 devices[] 是空的，而且{}。"
                      "空 devices[] 的配置要配合 --from-db 使用（采集内容来自编译快照）。",
                      cfgPath, fromDb ? "编译快照也没还原出设备" : "没有加 --from-db");
        if (cfg.web.enabled)
            spdlog::error("（若只想开 Web 编辑设备规格/设备树，仍需至少一个设备条目）");
        return 1;
    }

    // 采集源定下来之后才同步给 SharedState —— 采集线程一律按 id 去 state.config
    // 里现取配置（热切换的前提）。若还留着上面那份 config.json 的 devices[]，
    // --from-db 下每个线程都会"查无此任务"当场退出，进程活着却一个点都不采。
    state.config = cfg;
    initDeviceStats(cfg.devices, state);

    {
        int n_entries = 0, n_meters = 0;
        for (const auto& e : cfg.devices) {
            if (!e.enabled) continue;
            ++n_entries;
            if (e.protocol == industrial::Protocol::DLT645)
                n_meters += (int)e.dlt645.meters.size();
            else if (e.protocol == industrial::Protocol::DLT698)
                n_meters += (int)e.dlt698.meters.size();
        }
        spdlog::info("设备条目: {}  其中 DLT 电表: {}  配置: {}",
                     n_entries, n_meters, cfgPath);
    }

    // 采集线程池：先声明，热切换回调要往里加线程
    TaskThreads tt;

    // MQTT 在下面才构造，但热切换回调要捕获它 —— 用指针延迟绑定。
    industrial::ChannelBus* bus_ptr = nullptr;

    // ── 「生效」热切换 ─────────────────────────────────────────────────────
    // Web 点「生效」→ compileTree 写出新版本 → 这里把新快照换进 state.config
    // 并递增 reload_seq，采集线程各自结束本次连接、用新点表重建。
    // 失败一律不动 state.config：编译版本已经存进库里了，大不了重启生效，
    // 但绝不能因为快照有问题就把正在跑的配置清空。
    industrial::ReloadFn onReload;
    if (fromDb && cfgdb) {
        onReload = [&](int64_t ver, std::string& msg) -> bool {
            std::string rjson, rerr;
            industrial::RuntimeStats rst;
            if (!industrial::buildRuntimeJson(*cfgdb, ver, rjson, rst, rerr)) {
                msg = "热切换失败（运行中的配置未改动）：" + rerr;
                return false;
            }
            industrial::AppConfig nc;
            try { nc = industrial::appConfigFromJsonString(rjson); }
            catch (const std::exception& e) {
                msg = std::string("热切换失败（运行中的配置未改动）：快照无法解析 ")
                    + e.what();
                return false;
            }

            std::vector<std::string> added;
            {
                std::lock_guard<std::mutex> lk(state.config_mutex);
                std::set<std::string> before;
                for (const auto& e : state.config.devices) before.insert(e.id);
                for (const auto& e : nc.devices)
                    if (e.enabled && !before.count(e.id)) added.push_back(e.id);

                state.config.devices = nc.devices;
                state.runtime_version.store(ver, std::memory_order_release);
                // 换表之后、放锁之前递增：采集线程持同一把锁成对读出
                // (entry, seq)，因此不存在"拿到新表却记下旧世代"的组合。
                state.reload_seq.fetch_add(1, std::memory_order_acq_rel);
            }
            // 不在这里清理消失任务的统计：此刻它们的线程还没察觉世代变了，
            // 下一轮采集会把刚抹掉的行原样写回来。清理交给退出中的线程自己做
            // （SharedState::removeDevice），那个顺序才是确定的。
            initDeviceStats(nc.devices, state);

            for (const auto& id : added) {
                spdlog::info("[{}] 新增任务，启动采集线程", id);
                if (bus_ptr) spawnTask(tt, id, *bus_ptr, state);
            }

            msg = "已热切换到 v" + std::to_string(ver) + "：任务 "
                + std::to_string(rst.tasks) + " / 测点 " + std::to_string(rst.points);
            if (!added.empty()) msg += "，新增任务 " + std::to_string(added.size()) + " 个";
            spdlog::info("{}", msg);
            return true;
        };
    } else if (cfgdb) {
        // 没加 --from-db = 用户明确选了 config.json 作采集源，不能因为点了
        // 「生效」就把他悄悄切到库上。只如实告知怎样才会生效。
        onReload = [](int64_t ver, std::string& msg) -> bool {
            msg = "编译版本 v" + std::to_string(ver)
                + " 已保存；当前采集源是 config.json，重启时加 --from-db 方可生效";
            return true;
        };
    }

    // Web 服务器
    industrial::WebServer web(state, cfg.web, cfgdb.get(), onReload);
    if (cfg.web.enabled) {
        web.start();
        spdlog::info("Web 管理: http://{}:{}", cfg.web.bind, cfg.web.port);
    }

    // 本地缓存（声明在 mqtt 之前 → 析构在 mqtt 之后：回放线程先停再毁缓存）
    std::unique_ptr<industrial::DataCache> cache;
    if (cfg.cache.enabled) {
        cache = std::make_unique<industrial::DataCache>(
            cfg.cache.db_path, cfg.cache.retention_hours, cfg.cache.max_rows);
    }

    // ── 通道总线 ────────────────────────────────────────────────────────────
    // 通道来源优先级：
    //   1) config.db 的 channels 表非空 → 用它（真正的多通道）
    //   2) 空表 → 从 config.json.mqtt 造一个默认 MQTT 通道（向后兼容）
    // 空表默认写入 MQTT 一条，是因为在 P3.2 之前所有部署都是 config.json 单
    // MQTT，突然要用户先去通道页配一条才能启动会打断一切现场升级。这条自动
    // 通道有明确的 id "mqtt-default"，之后在 Web 上编辑即可。
    industrial::ChannelBus bus;

    // buildChannels：把通道表读一遍，转成一组 IChannel。空表就补一条默认 MQTT。
    // 提出来是为了让"启动"和"热切换"走同一段代码 —— reset() 拿走这份 vector 再
    // 分派 connect / disconnect。
    auto buildChannels = [&]() {
        std::vector<std::unique_ptr<industrial::IChannel>> out;
        std::vector<industrial::ChannelRow> chs = cfgdb ? cfgdb->listChannels()
                                                        : std::vector<industrial::ChannelRow>{};
        size_t nUsed = 0;
        for (const auto& c : chs) {
            if (!c.enabled) continue;
            if (c.direction == "down") continue;      // 纯下行走指令通路，不生成上行通道
            try {
                nlohmann::json j = nlohmann::json::parse(c.config_json);
                if (c.type == "mqtt") {
                    industrial::MqttChannelConfig mc;
                    mc.id           = c.id;
                    mc.broker       = j.value("broker", cfg.mqtt.broker);
                    mc.port         = j.value("port",   cfg.mqtt.port);
                    mc.client_id    = j.value("client_id", cfg.mqtt.client_id + "_" + c.id);
                    mc.username     = j.value("username", std::string());
                    mc.password     = j.value("password", std::string());
                    mc.topic_prefix = j.value("topic_prefix", cfg.mqtt.topic_prefix);
                    mc.qos          = j.value("qos",    cfg.mqtt.qos);
                    mc.retain       = j.value("retain", cfg.mqtt.retain);
                    mc.keepalive    = j.value("keepalive", cfg.mqtt.keepalive);
                    out.push_back(std::make_unique<industrial::MqttChannel>(mc));
                    ++nUsed;
                } else if (c.type == "file") {
                    industrial::FileChannelConfig fc;
                    fc.id        = c.id;
                    fc.dir       = j.value("dir", std::string("./channel_out/") + c.id);
                    fc.rotate_mb = j.value("rotate_mb", 0);
                    fc.keep      = j.value("keep", 5);
                    out.push_back(std::make_unique<industrial::FileChannel>(fc));
                    ++nUsed;
                } else {
                    spdlog::warn("[通道 {}] 未知类型 {}（跳过；P3.2c 加 kafka）",
                                 c.id, c.type);
                }
            } catch (const std::exception& e) {
                spdlog::warn("[通道 {}] 配置解析失败: {}（跳过）", c.id, e.what());
            }
        }
        if (nUsed == 0) {
            industrial::MqttChannelConfig mc;
            mc.id           = "mqtt-default";
            mc.broker       = cfg.mqtt.broker;
            mc.port         = cfg.mqtt.port;
            mc.client_id    = cfg.mqtt.client_id;
            mc.username     = cfg.mqtt.username;
            mc.password     = cfg.mqtt.password;
            mc.topic_prefix = cfg.mqtt.topic_prefix;
            mc.qos          = cfg.mqtt.qos;
            mc.retain       = cfg.mqtt.retain;
            mc.keepalive    = cfg.mqtt.keepalive;
            out.push_back(std::make_unique<industrial::MqttChannel>(mc));
        }
        return out;
    };

    // ── 下行任务级启停（P3.2b）─────────────────────────────────────────────
    // 现场从 MQTT 上发 `<prefix>/cmd/task/<id>/start`（或 stop）就能就地启停
    // 一个采集任务，不必登 Web。落库 → 拉起新配置 → 递增 reload_seq —— 与
    // P2c 「生效」走同一条通路，采集线程感知不到差别（"配置又变了，重来"）。
    // ack 打回 `<prefix>/cmd/task/<id>/ack`，含 ok/error/msg 便于自动化对账。
    // write_point（点级写）留给 P3.2c 或更晚 —— 那要按协议加权限控制，是另一件事。
    auto onTaskCmd = [&](
                        const std::string& mqtt_prefix,
                        const std::string& topic, const std::string& /*payload*/) {
        // ack 也匹配 `<prefix>/cmd/task/#`，如果不挡就会形成【自触发环】：
        // 我们回一条 ack → broker 分发回来 → 又当成"未知动作 ack" → 再回一条
        // error ack → 一直循环。实测头一版正是这个症状。ack 主题直接静默返回。
        if (topic.size() >= 4 && topic.compare(topic.size() - 4, 4, "/ack") == 0) return;
        // topic = <mqtt_prefix>/cmd/task/<id>/<action>
        const std::string ack_topic_base = mqtt_prefix + "/cmd/task/";
        auto ackErr = [&](const std::string& tid, const std::string& why) {
            spdlog::warn("[下行] {}", why);
            nlohmann::json j = {{"ok", false}, {"error", why}};
            bus.publishTopic(ack_topic_base + tid + "/ack", j.dump(), 1, false);
        };

        // 剥前缀 `<mqtt_prefix>/cmd/task/`
        const std::string head = mqtt_prefix + "/cmd/task/";
        if (topic.rfind(head, 0) != 0) return;   // 前缀不对（订阅乱撒的兜底）
        const std::string tail = topic.substr(head.size());   // "<id>/<action>"
        const auto slash = tail.rfind('/');
        if (slash == std::string::npos) { ackErr("?", "指令主题格式错: " + topic); return; }
        const std::string task_id = tail.substr(0, slash);
        const std::string action  = tail.substr(slash + 1);

        if (action != "start" && action != "stop") {
            ackErr(task_id, "未知动作: " + action + "（支持 start/stop）");
            return;
        }
        const bool want = (action == "start");

        // 1) 修改 state.config.devices —— 用 P2c 那对成对读写模式
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(state.config_mutex);
            for (auto& d : state.config.devices) {
                if (d.id != task_id) continue;
                if (d.enabled != want) { d.enabled = want; changed = true; }
                break;
            }
            if (changed) state.reload_seq.fetch_add(1, std::memory_order_acq_rel);
        }
        // 2) 落库（若有 config.db），下次重启保持一致
        if (cfgdb && cfgdb->ok()) {
            for (auto t : cfgdb->listTasks()) {
                if (t.id == task_id) { t.enabled = want; cfgdb->upsertTask(t); break; }
            }
        }
        // 3) 启动被停用过的任务：spawnTask 幂等，不会重复起
        if (changed && want) spawnTask(tt, task_id, bus, state);

        // 4) ack
        nlohmann::json ok = {{"ok", true}, {"task", task_id}, {"action", action},
                             {"changed", changed}};
        bus.publishTopic(ack_topic_base + task_id + "/ack", ok.dump(), 1, false);
        spdlog::info("[下行] 任务 {} → {}（{}）", task_id, action,
                     changed ? "已生效" : "状态未变");
    };

    // afterStart：新一套通道 connect 完毕后要做的两件事 —— 挂缓存 + 订下行。
    // 启动时跑一次，热切换后再跑一次；两条路走同一段代码。
    auto afterStart = [&](industrial::ChannelBus& b) {
        // 1) 缓存挂到"第一个 MQTT 上行通道" —— 多通道续传语义未定，硬做会踩坑
        //    （同一份缓存回放到两个 broker 会重复消费）。用户排序决定谁挂缓存。
        industrial::MqttChannel* prim = nullptr;
        if (cfgdb) {
            for (const auto& c : cfgdb->listChannels()) {
                if (c.type == "mqtt" && c.enabled && c.direction != "down") {
                    if (auto* ch = b.find(c.id)) {
                        prim = static_cast<industrial::MqttChannel*>(ch);
                        break;
                    }
                }
            }
        }
        if (!prim) prim = static_cast<industrial::MqttChannel*>(b.find("mqtt-default"));
        if (prim && cache) prim->attachCache(cache.get(), cfg.cache);

        // 2) 下行指令订阅：每个 MQTT 通道各自订自己前缀。多通道 = 多个入口，
        //    但状态只写一份（onTaskCmd 里靠 changed=false 去重）。
        auto subscribeIfMqtt = [&](const std::string& id, const std::string& prefix) {
            auto* ch = b.find(id);
            if (!ch || ch->type() != "mqtt") return;
            static_cast<industrial::MqttChannel*>(ch)->subscribeCmd(
                prefix + "/cmd/task/#",
                [onTaskCmd, prefix](const std::string& t, const std::string& p) {
                    onTaskCmd(prefix, t, p);
                });
        };
        if (cfgdb) {
            for (const auto& c : cfgdb->listChannels()) {
                if (c.type != "mqtt" || !c.enabled || c.direction == "down") continue;
                try {
                    nlohmann::json j = nlohmann::json::parse(c.config_json);
                    subscribeIfMqtt(c.id, j.value("topic_prefix", cfg.mqtt.topic_prefix));
                } catch (...) {}
            }
        }
        subscribeIfMqtt("mqtt-default", cfg.mqtt.topic_prefix);

        // 【注意】subscribeCmd 存的是订阅意向；实际 client_->subscribe 发生在
        // onConnected() 里。但 reset() 已经在锁内触发 connect() 完成，此时
        // 上面这些 subscribeCmd 追加进去的意向还没订上 —— 手动触发一次。
        // 首次启动时 onConnected 已订过缓存主题；再调一次会重订同一堆主题，
        // paho 用最后一次订阅的 QoS 覆盖，语义安全。
        for (const auto& c : (cfgdb ? cfgdb->listChannels()
                                    : std::vector<industrial::ChannelRow>{})) {
            if (c.type == "mqtt" && c.enabled) {
                if (auto* ch = b.find(c.id))
                    static_cast<industrial::MqttChannel*>(ch)->onConnected();
            }
        }
        if (auto* ch = b.find("mqtt-default"))
            static_cast<industrial::MqttChannel*>(ch)->onConnected();
    };

    // 启动：走同一段 reset。失败没什么可做 —— 单个通道失败已在 reset 里降级，
    // 整个 reset 只在传入空集合等奇葩情形下才有异常，那就不该继续跑。
    try {
        bus.reset(buildChannels(), afterStart);
    } catch (const std::exception& e) {
        spdlog::critical("通道启动失败: {}", e.what());
        if (cfg.web.enabled) web.stop();
        return 1;
    }
    spdlog::info("通道总线: {} 个通道", bus.size());

    // 通道热切换 —— 前端点保存后调 POST /api/channels/reload
    web.setChannelsReload([&](std::string& msg) -> bool {
        try {
            auto newChs = buildChannels();
            const size_t n = newChs.size();
            bus.reset(std::move(newChs), afterStart);
            msg = "通道已热切换：" + std::to_string(n) + " 个通道就位";
            spdlog::info("{}", msg);
            return true;
        } catch (const std::exception& e) {
            msg = std::string("通道热切换失败：") + e.what();
            spdlog::error("{}", msg);
            return false;
        }
    });

    bus_ptr = &bus;   // 热切换回调此时才能安全地拉起新线程

    // 每个启用的 DeviceEntry 启动一个线程
    for (const auto& entry : cfg.devices) {
        if (!entry.enabled) {
            spdlog::info("[{}] 已禁用，跳过", entry.id);
            continue;
        }
        spawnTask(tt, entry.id, bus, state);
    }

    // join：热切换会往 tt.threads 里追加（新增任务），所以不能对着一份快照迭代，
    // 按下标推进、每轮重读 size()。而且必须把句柄【移出】vector 再 join：
    // 持锁 join 会和 spawnTask 死锁，不持锁又可能撞上 push_back 触发的扩容搬移。
    for (size_t i = 0;; ++i) {
        std::thread th;
        {
            std::lock_guard<std::mutex> lk(tt.mtx);
            if (i >= tt.threads.size()) break;
            th = std::move(tt.threads[i]);
        }
        if (th.joinable()) th.join();
    }

    bus.stopAll();
    if (cfg.web.enabled) web.stop();
    spdlog::info("所有设备线程已退出，程序正常结束");
    return 0;
}
