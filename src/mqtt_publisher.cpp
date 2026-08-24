// src/mqtt_publisher.cpp
#include "mqtt_publisher.h"
#include <mqtt/async_client.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <chrono>

using json = nlohmann::json;
namespace industrial {

// ── 回调 ─────────────────────────────────────────────────────────────────────
class MqttCb : public mqtt::callback, public mqtt::iaction_listener {
public:
    MqttCb(std::atomic<bool>& conn,
           mqtt::async_client& cli,
           mqtt::connect_options& opts,
           MqttPublisher& owner)
        : conn_(conn), cli_(cli), opts_(opts), owner_(owner) {}

    void connection_lost(const std::string& cause) override {
        conn_ = false;
        spdlog::warn("MQTT 断开: {}", cause.empty() ? "未知" : cause);
        try { cli_.connect(opts_, nullptr, *this); }
        catch (...) { spdlog::error("MQTT 重连失败"); }
    }
    void connected(const std::string&) override {
        conn_ = true;
        spdlog::info("MQTT 已连接");
        owner_.onConnected();   // (重)订阅指令主题
    }
    void on_failure(const mqtt::token&) override { conn_ = false; }
    void on_success(const mqtt::token&) override {}
    void message_arrived(mqtt::const_message_ptr msg) override {
        if (msg) owner_.onCommand(msg->get_topic(), msg->to_string());
    }
    void delivery_complete(mqtt::delivery_token_ptr) override {}

private:
    std::atomic<bool>&     conn_;
    mqtt::async_client&    cli_;
    mqtt::connect_options& opts_;
    MqttPublisher&         owner_;
};

// ── 构造 / 析构 ───────────────────────────────────────────────────────────────
MqttPublisher::MqttPublisher(const MqttConfig& cfg) : cfg_(cfg) {
    std::string server = "tcp://" + cfg.broker + ":" + std::to_string(cfg.port);
    // 客户端侧缓冲：paho 默认 0，一断线立刻 [-3]；publishPoints 每轮上万条时
    // 缓冲不够就 [-12]（No more messages can be buffered）。给它一个够大的
    // 上限（8192），真正的背压交给 publishPoints 里的等待循环 —— 这里只是
    // 把"瞬间抖动"扛过去，不承担"上游产得比下游发得快"这种系统性压力。
    auto co = mqtt::create_options_builder()
        .max_buffered_messages(8192)
        .finalize();
    client_ = std::make_unique<mqtt::async_client>(server, cfg.client_id,
                                                    co, std::string());

    auto b = mqtt::connect_options_builder()
        .keep_alive_interval(std::chrono::seconds(cfg.keepalive))
        .clean_session(true)
        .automatic_reconnect(std::chrono::seconds(2), std::chrono::seconds(30));
    if (!cfg.username.empty())
        b.user_name(cfg.username).password(cfg.password);
    connOpts_ = std::make_unique<mqtt::connect_options>(b.finalize());
}

MqttPublisher::~MqttPublisher() {
    try { disconnect(); } catch (...) {}
}

void MqttPublisher::attachCache(DataCache* cache, const CacheConfig& cc) {
    cache_    = cache;
    cacheCfg_ = cc;
    cacheOn_  = (cache != nullptr && cache->ok() && cc.enabled);
}

void MqttPublisher::connect() {
    auto cb = std::make_shared<MqttCb>(connected_, *client_, *connOpts_, *this);
    cb_ = cb;                       // 成员持有，保证回调对象存活到发布器销毁
    client_->set_callback(*cb);
    try {
        client_->connect(*connOpts_)->wait_for(std::chrono::seconds(10));
        connected_ = true;
        spdlog::info("MQTT 连接成功 {}:{}", cfg_.broker, cfg_.port);
    } catch (const mqtt::exception& e) {
        throw std::runtime_error(std::string("MQTT 连接失败: ") + e.what());
    }
    onConnected();   // 首次连接也订阅（connected 回调时序不确定，幂等）

    // 启动回放线程
    if (cacheOn_ && !replayThread_.joinable()) {
        stop_ = false;
        replayThread_ = std::thread(&MqttPublisher::replayLoop, this);
    }
}

void MqttPublisher::disconnect() {
    // 先停回放线程
    if (replayThread_.joinable()) {
        stop_ = true;
        reqCv_.notify_all();
        replayThread_.join();
    }
    if (client_ && client_->is_connected()) {
        client_->disconnect()->wait();
        connected_ = false;
        spdlog::info("MQTT 已断开");
    }
}

// ── 订阅指令主题（首连 + 每次重连都要，clean_session 会清订阅）──────────────
void MqttPublisher::onConnected() {
    if (cacheOn_) {
        try {
            client_->subscribe(cacheCfg_.command_topic, 1);
            spdlog::info("已订阅续传指令主题: {}", cacheCfg_.command_topic);
        } catch (const std::exception& e) {
            spdlog::warn("订阅续传指令主题失败: {}", e.what());
        }
    }
    for (const auto& s : cmdSubs_) {
        try {
            client_->subscribe(s.filter, 1);
            spdlog::info("已订阅下行指令主题: {}", s.filter);
        } catch (const std::exception& e) {
            spdlog::warn("订阅下行指令主题 {} 失败: {}", s.filter, e.what());
        }
    }
}

void MqttPublisher::subscribeCmd(const std::string& topic_filter, CmdHandler handler) {
    cmdSubs_.push_back({topic_filter, std::move(handler)});
}

// MQTT 通配符匹配：'+' 单层任意，'#' 尾端多层任意。手写而不用 mosquitto 的实现，
// 因为 paho 客户端只做"服务端订阅匹配"，收到消息后本地要按 filter 分流还得自己
// 判断。逻辑简单，实测已足以：`test/docker/cmd/task/+/start` 应命中
// `test/docker/cmd/task/modbus_sim/start`。
bool MqttPublisher::topicMatch(const std::string& f, const std::string& t) {
    size_t i = 0, j = 0;
    while (i < f.size() && j < t.size()) {
        if (f[i] == '#') return true;               // 尾端多层，直接吞
        if (f[i] == '+') {                          // 单层：t 里下一个 '/' 之前都算
            while (j < t.size() && t[j] != '/') ++j;
            ++i;                                    // 跳过 '+'
            if (i < f.size() && f[i] == '/' && j < t.size() && t[j] == '/') { ++i; ++j; }
            continue;
        }
        if (f[i] != t[j]) return false;
        ++i; ++j;
    }
    if (i < f.size() && f[i] == '#') return true;
    return i == f.size() && j == t.size();
}

// ── 收到指令：解析后入队，重活交回放线程 ────────────────────────────────────
void MqttPublisher::onCommand(const std::string& topic, const std::string& payload) {
    // 下行指令订阅优先（任务级启停等）；未命中再看是否是续传指令
    for (const auto& s : cmdSubs_) {
        if (topicMatch(s.filter, topic)) { s.handler(topic, payload); return; }
    }
    if (!cacheOn_ || topic != cacheCfg_.command_topic) return;
    Req req;
    req.mode = Mode::RESUME; req.since = 0;
    try {
        auto j = json::parse(payload.empty() ? "{}" : payload);
        if (j.contains("start") && j.contains("end")) {
            req.mode  = Mode::RANGE;
            req.start = j["start"].get<std::string>();
            req.end   = j["end"].get<std::string>();
        } else if (j.contains("since")) {
            req.mode  = Mode::SINCE;
            req.since = j["since"].get<int64_t>();
        }
    } catch (const std::exception& e) {
        spdlog::warn("续传指令解析失败，按普通续传处理: {}", e.what());
    }
    {
        std::lock_guard<std::mutex> lk(reqMtx_);
        reqQ_.push_back(req);
    }
    reqCv_.notify_one();
    spdlog::info("收到续传指令 mode={}",
        req.mode==Mode::RESUME ? "resume" : req.mode==Mode::SINCE ? "since" : "range");
}

// ── 回放线程主循环 ────────────────────────────────────────────────────────────
void MqttPublisher::replayLoop() {
    while (!stop_) {
        Req req;
        {
            std::unique_lock<std::mutex> lk(reqMtx_);
            reqCv_.wait(lk, [this]{ return stop_ || !reqQ_.empty(); });
            if (stop_) break;
            req = reqQ_.front(); reqQ_.pop_front();
        }
        doReplay(req);
    }
}

void MqttPublisher::doReplay(const Req& req) {
    const std::string& htopic = cacheCfg_.history_topic;
    const int chunk = cacheCfg_.replay_chunk > 0 ? cacheCfg_.replay_chunk : 200;
    int64_t cursor = (req.mode == Mode::RESUME) ? cache_->sendCursor()
                   : (req.mode == Mode::SINCE)  ? req.since : 0;
    int64_t lastSent = cursor;
    int64_t count = 0;

    while (!stop_) {
        std::vector<CacheRow> rows = (req.mode == Mode::RANGE)
            ? cache_->queryRange(req.start, req.end, cursor, chunk)
            : cache_->querySince(cursor, chunk);
        if (rows.empty()) break;

        for (const auto& r : rows) {
            if (stop_) return;
            if (!connected_) {       // 断线则停，留待重新指令；不推进断点
                spdlog::warn("回放中断（MQTT 断开），已发 {} 条", count);
                if (req.mode == Mode::RESUME) cache_->setSendCursor(lastSent);
                return;
            }
            json w;
            w["cursor"] = r.id;          // 断点游标
            w["device"] = r.device_id;
            w["ts"]     = r.ts;
            w["topic"]  = r.topic;
            try { w["data"] = json::parse(r.payload); }
            catch (...) { w["data"] = r.payload; }
            publish(htopic, w.dump(), cfg_.qos, false);

            cursor = r.id; lastSent = r.id; ++count;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(cacheCfg_.replay_rate_ms));
        }
        // 普通续传：分块推进持久化断点
        if (req.mode == Mode::RESUME) cache_->setSendCursor(lastSent);
    }

    // 结束标记
    json done;
    done["_replay"] = "done";
    done["cursor"]  = lastSent;
    done["count"]   = count;
    publish(htopic, done.dump(), cfg_.qos, false);
    spdlog::info("续传完成：{} 条，末游标 {}", (long long)count, (long long)lastSent);
}

// 发布失败：首条即时报（带主题与原因，便于定位），随后每秒汇总一次条数。
// 判据是"看得见问题、又不至于把日志冲掉"，所以汇总行里保留最后一条的主题和原因。
void MqttPublisher::noteFailure(const std::string& topic, const std::string& what) {
    std::lock_guard<std::mutex> lk(failMtx_);
    const auto now = std::chrono::steady_clock::now();
    ++failCount_;
    failLastTopic_ = topic;
    failLastWhat_  = what;

    if (failLastReport_.time_since_epoch().count() == 0) {
        spdlog::error("MQTT 发布失败 {}: {}", topic, what);
        failLastReport_ = now;
        failCount_ = 0;
        return;
    }
    if (now - failLastReport_ >= std::chrono::seconds(1)) {
        if (failCount_ > 0)
            spdlog::error("MQTT 发布失败 {} 条（最近 {}: {}）",
                          (unsigned long long)failCount_, failLastTopic_, failLastWhat_);
        failLastReport_ = now;
        failCount_ = 0;
    }
}

// ── 实时发布 ─────────────────────────────────────────────────────────────────
//
// 背压：paho 的 async 队列一旦爆（默认 0，我们提到 8192）就抛 [-12]。ESS 站
// 13084 点 × 2s 周期，逐条发大概率打爆队列，实测一轮丢 4000 多条 —— 有 0 错误
// 日志但下游数据不完整，是最难查的那类"错"。
//
// 这里的选择是"等一小段时间再重试一次"：paho 内部处理完几条就腾出位置了，
// 短暂的背压是可以吸收的；持久压力再退化为聚合日志的丢弃。不引入 async 回调
// 的完成计数是有意的 —— 那会把 MqttCb / 采集线程 / 发布主线程搅在一起，复杂度
// 与本次修复不匹配（真正的多通道背压留给 IChannel 抽出来后再谈）。
void MqttPublisher::publish(const std::string& topic,
                             const std::string& payload,
                             int qos, bool retain) {
    if (!connected_) return;     // 离线丢弃（实时通道）；缓存在 publishPoints 已写
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            client_->publish(mqtt::make_message(topic, payload, qos, retain));
            return;
        } catch (const mqtt::exception& e) {
            // MQTTASYNC_MAX_BUFFERED_MESSAGES = -12 —— 队列满时短暂等待重试
            if (e.get_reason_code() == -12 && attempt == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            noteFailure(topic, e.what());
            return;
        }
    }
}

void MqttPublisher::publishPoints(const DataPoints& points,
                                   const std::string& prefix,
                                   int qos, bool retain,
                                   const std::string& publish_mode,
                                   const std::vector<ModbusRange>& ranges) {
    if (publish_mode == "array" && !ranges.empty()) {
        // ── Array 模式：每个 range 发一条紧凑数组消息 ──────────────────────
        // 1) 手配命名点：照常发单点主题 + 写缓存
        for (const auto& p : points) {
            if (p.from_range) continue;
            publish(prefix + "/" + p.name, buildPointJson(p), qos, retain);
        }

        // 2) 每个 range 发一条数组消息，并写缓存
        for (const auto& r : ranges) {
            json arr;
            arr["ts"]          = nowIso();
            arr["name_prefix"] = r.name_prefix;
            arr["count"]       = r.count;
            arr["scale"]       = r.scale;
            arr["unit"]        = r.unit;
            json vals = json::array();
            for (const auto& p : points) {
                if (!p.from_range) continue;
                if (p.name.substr(0, r.name_prefix.size()) != r.name_prefix) continue;
                // 存原始整数值（接收端乘以 scale 还原工程量）
                auto rv = std::visit([](auto&& v) -> json {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>) return nullptr;
                    else return v;
                }, p.raw_value);
                vals.push_back(rv);
            }
            arr["values"] = vals;
            std::string payload = arr.dump();

            // topic: {prefix}/{name_prefix 去掉末尾 _}
            std::string rname = r.name_prefix;
            if (!rname.empty() && rname.back() == '_') rname.pop_back();
            std::string topic = prefix + "/" + rname;

            if (cacheOn_)
                cache_->store(prefix + "/" + rname, nowIso(), topic, payload);
            publish(topic, payload, qos, retain);
        }
    } else {
        // ── Batch 模式（默认）────────────────────────────────────────────────
        // 【全轮共用同一个 timestamp】—— 分片方案的语义关键点。分片各自算
        // nowIso() 的话，同一轮的分片时间戳互不相同，下游按 timestamp 分桶
        // 就永远拼不出完整的一轮，看到的永远是"上一轮的 X 片 + 这一轮的 Y 片"，
        // 也就永远算不对包电压之和 = 24 电芯 —— 与丢包一样是"绿着算错"。
        const std::string ts = nowIso();

        // 完整快照 → prefix/batch，续传时能整轮回放。始终发一次。
        std::string batch = buildBatchJson(points, ts);
        if (cacheOn_)
            cache_->store(prefix, ts, prefix + "/batch", batch);
        publish(prefix + "/batch", batch, qos, retain);

        // 逐点主题：低点数（如 modbus_sim 3 个点）时保留 —— 现场既有订阅按
        // prefix/point_name 拿单点是主流用法。但 ESS 站 13084 点全走逐点会
        // 打爆客户端队列，实测一轮丢 4000 多条。分片成 N 点/帧的批量发布，
        // 消息数从 O(点数) 降到 O(点数/N)。
        //
        // 阈值 100：小设备（Modbus 3 点 / DLT 5 点 / 常见 SCADA 站几十点）
        // 依然逐点发，下游订阅不动；上千点的规格化设备走分片，只多出
        // prefix/batch/000 这类分片主题，语义上仍是"一整轮"。
        constexpr size_t PER_TOPIC_MAX  = 100;
        constexpr size_t CHUNK_SIZE     = 200;    // 每片 200 点，序列化后 <32KB
        if (points.size() <= PER_TOPIC_MAX) {
            for (const auto& p : points)
                publish(prefix + "/" + p.name, buildPointJson(p), qos, retain);
        } else {
            // 分片：把 points 切成若干段，每段一条 batch 消息
            // 主题 prefix/batch/000 保留三位数编号：接收端排序、且不与
            // prefix/batch 冲突（后者是全量快照，前者是分片）。
            const size_t nchunks = (points.size() + CHUNK_SIZE - 1) / CHUNK_SIZE;
            char sub[24];   // 3 位数编号够用；宽点绕过 %03zu 的 -Wformat-truncation
            for (size_t i = 0; i < nchunks; ++i) {
                const size_t b = i * CHUNK_SIZE;
                const size_t e = std::min(b + CHUNK_SIZE, points.size());
                DataPoints slice(points.begin() + b, points.begin() + e);
                std::snprintf(sub, sizeof(sub), "%03zu", i);
                const std::string topic   = prefix + "/batch/" + sub;
                const std::string payload = buildBatchJson(slice, ts);
                publish(topic, payload, qos, retain);
            }
        }
    }
}

std::string MqttPublisher::buildPointJson(const DataPoint& p) {
    json j;
    j["name"]        = p.name;
    j["quality"]     = qualityStr(p.quality);
    j["timestamp"]   = p.timestamp;
    j["unit"]        = p.unit;
    j["description"] = p.description;
    auto v = p.toDouble();
    j["value"]     = v.has_value()  ? json(*v)  : json(nullptr);
    auto r = std::visit([](auto&& x) -> std::optional<double> {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return std::nullopt;
        else return static_cast<double>(x);
    }, p.raw_value);
    j["raw_value"] = r.has_value() ? json(*r) : json(nullptr);
    return j.dump();
}

std::string MqttPublisher::buildBatchJson(const DataPoints& pts,
                                          const std::string& ts) {
    json j;
    j["timestamp"] = ts;
    for (const auto& p : pts) {
        auto v = p.toDouble();
        j["points"][p.name] = {
            {"value",   v.has_value() ? json(*v) : json(nullptr)},
            {"quality", qualityStr(p.quality)},
            {"unit",    p.unit}
        };
    }
    return j.dump();
}

} // namespace industrial
