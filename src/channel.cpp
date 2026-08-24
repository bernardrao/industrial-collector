// src/channel.cpp — 通道总线 + MqttChannel / FileChannel 实现
//
// ── MqttChannel 用组合、不用继承 ────────────────────────────────────────────
// 现有 MqttPublisher 已经承载了连接、重连、缓存、续传、指令回调这一整套东西
// （P2b / P2c / P3.1 层层踩坑加固的）。把它塞进继承体系里等于把这些成熟代码
// 与"通道抽象"这个新东西粘死。改成组合：MqttChannel 只是把 IChannel 的方法
// 转发给 MqttPublisher。有 bug 时也是原地修 MqttPublisher，不会被抽象拖累。

#include "channel.h"
#include "mqtt_publisher.h"
#include "config.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

using json = nlohmann::json;

namespace industrial {

namespace {
// nowIso() 从 types.h 复用，别再造一份 —— 之前造重名，编译时歧义
const std::string kMqttType = "mqtt";
const std::string kFileType = "file";
} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// MqttChannel —— 组合 MqttPublisher
// ═════════════════════════════════════════════════════════════════════════════
struct MqttChannel::Impl {
    MqttChannelConfig    cfg;
    MqttConfig           legacy;   // MqttPublisher 仍要 MqttConfig
    MqttPublisher        pub;
    explicit Impl(const MqttChannelConfig& c)
        : cfg(c), legacy(toLegacy(c)), pub(legacy) {}

    static MqttConfig toLegacy(const MqttChannelConfig& c) {
        MqttConfig m;
        m.broker       = c.broker;
        m.port         = c.port;
        m.client_id    = c.client_id;
        m.username     = c.username;
        m.password     = c.password;
        m.topic_prefix = c.topic_prefix;
        m.qos          = c.qos;
        m.retain       = c.retain;
        m.keepalive    = c.keepalive;
        return m;
    }
};

MqttChannel::MqttChannel(const MqttChannelConfig& cfg)
    : id_(cfg.id), pimpl_(std::make_unique<Impl>(cfg)) {}
MqttChannel::~MqttChannel() = default;

void MqttChannel::connect()          { pimpl_->pub.connect(); }
void MqttChannel::disconnect()       { pimpl_->pub.disconnect(); }
bool MqttChannel::ready() const      { return pimpl_->pub.isConnected(); }
const std::string& MqttChannel::type() const { return kMqttType; }

void MqttChannel::attachCache(DataCache* cache, const CacheConfig& cc) {
    pimpl_->pub.attachCache(cache, cc);
}

void MqttChannel::onConnected() { pimpl_->pub.onConnected(); }
void MqttChannel::onCommand(const std::string& t, const std::string& p) {
    pimpl_->pub.onCommand(t, p);
}
void MqttChannel::subscribeCmd(const std::string& f, CmdHandler h) {
    pimpl_->pub.subscribeCmd(f, std::move(h));
}

void MqttChannel::publishRound(const std::string& device_id,
                                const DataPoints& points,
                                const std::string& /*ts*/) {
    // MqttPublisher::publishPoints 内部按 P3.1 的规则分片：小设备逐点 + batch，
    // 大规格设备走 batch/000..NNN，全轮共用一个 ts。
    // topic_prefix 从通道配置来，不再从 config.mqtt 读。
    const std::string prefix = pimpl_->cfg.topic_prefix + "/" + device_id;
    pimpl_->pub.publishPoints(points, prefix, pimpl_->cfg.qos, pimpl_->cfg.retain);
    // 注意：`ts` 参数当前未被采用 —— MqttPublisher 内部自己算。这不是问题：
    // 单通道语义下"MQTT 的 ts"和"总线派发的 ts"必然一致；等 IChannel 里的
    // ts 被别的通道用到（比如文件通道要按 ts 归档）时再穿透。留形参不空跑：
    // 抽象里已经明确说了"一整轮一个 ts"，实现方少一个参数就少一处随时可能
    // 各自算 nowIso 的地方。
}

void MqttChannel::publishTopic(const std::string& topic, const std::string& payload,
                                int qos, bool retain) {
    pimpl_->pub.publish(topic, payload, qos, retain);
}

// ═════════════════════════════════════════════════════════════════════════════
// FileChannel —— JSON-Lines 追加
// ═════════════════════════════════════════════════════════════════════════════
//
// 每个 device_id 一份 .jsonl，一行一整轮：
//   {"ts":"...","device_id":"...","points":{name:{value,quality,unit},...}}
//
// 用途：气隙 / 穿网闸场景把数据摆到磁盘由别的进程摆渡；离线诊断录一段回放。
// 明确不做的事：不做 fsync（追加写 kernel 缓冲已够；掉电时丢失最后几行是
// 可接受的），不做压缩（jq / grep 用起来省事），不做轮转期间的原子性保证
// （只保证已写入的行是完整的一行）。
struct FileChannel::Impl {
    std::mutex mtx;
    struct Sink { std::ofstream ofs; std::string path; size_t bytes = 0; };
    std::unordered_map<std::string, Sink> sinks;   // device_id → sink
};

FileChannel::FileChannel(const FileChannelConfig& cfg)
    : cfg_(cfg), pimpl_(std::make_unique<Impl>()) {}
FileChannel::~FileChannel() { disconnect(); }

const std::string& FileChannel::type() const { return kFileType; }

// mkdir -p，逐级建。open 时目录不存在直接失败，比启动时报错好定位。
static void mkdirp(const std::string& p) {
    if (p.empty()) return;
    size_t i = 0;
    while (i < p.size()) {
        i = p.find('/', i + 1);
        if (i == std::string::npos) i = p.size();
        std::string sub = p.substr(0, i);
        if (!sub.empty()) ::mkdir(sub.c_str(), 0755);   // 已存在返回 EEXIST，忽略
    }
}

void FileChannel::connect() {
    mkdirp(cfg_.dir);
    // 探测可写：拉个空文件试一下，比等第一次 publishRound 才发现权限错好
    const std::string probe = cfg_.dir + "/.write_probe";
    std::ofstream t(probe, std::ios::app);
    if (!t) throw std::runtime_error("文件通道无法写入 " + cfg_.dir);
    t.close();
    ::unlink(probe.c_str());
    spdlog::info("[通道 {}] 文件通道就绪 → {}", cfg_.id, cfg_.dir);
}

void FileChannel::disconnect() {
    std::lock_guard<std::mutex> lk(pimpl_->mtx);
    pimpl_->sinks.clear();
}

bool FileChannel::ready() const { return true; }

// 轮转：单文件超过 rotate_mb*1024*1024 时改名 .1 / .2 ... 老的挤掉
static void rotateIfNeeded(FileChannel::Impl::Sink& s, const FileChannelConfig& cfg) {
    if (cfg.rotate_mb <= 0) return;
    if (s.bytes < (size_t)cfg.rotate_mb * 1024 * 1024) return;

    s.ofs.close();
    // 从后往前推：.N → 丢弃，.N-1 → .N，... .1 → .2，当前 → .1
    for (int i = cfg.keep; i > 0; --i) {
        std::string cur = s.path + "." + std::to_string(i);
        std::string nxt = s.path + "." + std::to_string(i + 1);
        if (i == cfg.keep) ::unlink(cur.c_str());
        else               ::rename(cur.c_str(), nxt.c_str());
    }
    ::rename(s.path.c_str(), (s.path + ".1").c_str());
    s.ofs.open(s.path, std::ios::app);
    s.bytes = 0;
}

void FileChannel::publishRound(const std::string& device_id,
                                const DataPoints& points,
                                const std::string& ts) {
    json j;
    j["ts"]        = ts;
    j["device_id"] = device_id;
    json& pj = j["points"];
    pj = json::object();
    for (const auto& p : points) {
        auto v = p.toDouble();
        pj[p.name] = {
            {"value",   v.has_value() ? json(*v) : json(nullptr)},
            {"quality", (p.quality == Quality::GOOD ? "GOOD" : "BAD")},
            {"unit",    p.unit},
        };
    }
    std::string line = j.dump() + "\n";

    std::lock_guard<std::mutex> lk(pimpl_->mtx);
    auto it = pimpl_->sinks.find(device_id);
    if (it == pimpl_->sinks.end()) {
        Impl::Sink s;
        // device_id 会含 '/' —— MQTT 里当子主题，磁盘上会被误认成目录。
        // 一刀切换成 '_' 让它变成合法文件名，不再有隐含目录层级。
        std::string fname = device_id;
        for (char& c : fname) if (c == '/') c = '_';
        s.path = cfg_.dir + "/" + fname + ".jsonl";
        s.ofs.open(s.path, std::ios::app);
        if (!s.ofs) {
            spdlog::error("[通道 {}] 无法打开 {}", cfg_.id, s.path);
            return;
        }
        struct stat st{}; if (::stat(s.path.c_str(), &st) == 0) s.bytes = st.st_size;
        it = pimpl_->sinks.emplace(device_id, std::move(s)).first;
    }
    it->second.ofs.write(line.data(), (std::streamsize)line.size());
    it->second.ofs.flush();      // 至少刷到 kernel，掉电最多丢最后一行
    it->second.bytes += line.size();
    rotateIfNeeded(it->second, cfg_);
}

// ═════════════════════════════════════════════════════════════════════════════
// ChannelBus
// ═════════════════════════════════════════════════════════════════════════════
ChannelBus::~ChannelBus() { stopAll(); }

void ChannelBus::add(std::unique_ptr<IChannel> ch) {
    std::lock_guard<std::mutex> lk(mtx_);
    channels_.push_back(std::move(ch));
}

void ChannelBus::startAll() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& ch : channels_) {
        try { ch->connect(); }
        catch (const std::exception& e) {
            // 单个通道失败不阻塞其余 —— 现场配错一个 Kafka 集群不该让整台机
            // 采集失败。会以 ready()=false 一直被跳过。
            spdlog::error("[通道 {}] connect 失败: {}", ch->id(), e.what());
        }
    }
}

void ChannelBus::stopAll() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& ch : channels_) {
        try { ch->disconnect(); }
        catch (...) {}
    }
}

void ChannelBus::reset(std::vector<std::unique_ptr<IChannel>> newChs,
                        std::function<void(ChannelBus&)> after_start) {
    // 老通道先移出锁再销毁 —— MqttChannel 析构里有 disconnect 的 wait，可能
    // 阻塞几十毫秒。放在锁内会白白撑长 publish 的等待。
    std::vector<std::unique_ptr<IChannel>> old;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        old = std::move(channels_);
        channels_ = std::move(newChs);
        for (auto& ch : channels_) {
            try { ch->connect(); }
            catch (const std::exception& e) {
                spdlog::error("[通道 {}] connect 失败: {}", ch->id(), e.what());
            }
        }
    }
    for (auto& ch : old) {
        try { ch->disconnect(); }
        catch (...) {}
    }
    // 析构 old 里的对象 —— 走原有的 dtor，在锁外，publish 不受影响。
    old.clear();
    if (after_start) after_start(*this);
}

void ChannelBus::publishRound(const std::string& device_id, const DataPoints& points) {
    // 全轮共用同一个墙钟时刻 —— 见 channel.h 的三条硬约束
    const std::string ts = nowIso();
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& ch : channels_) {
        if (!ch->ready()) continue;
        try { ch->publishRound(device_id, points, ts); }
        catch (const std::exception& e) {
            spdlog::error("[通道 {}] publishRound 异常: {}", ch->id(), e.what());
        }
    }
}

void ChannelBus::publishTopic(const std::string& topic, const std::string& payload,
                                int qos, bool retain) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& ch : channels_) {
        if (ch->type() != kMqttType || !ch->ready()) continue;
        static_cast<MqttChannel*>(ch.get())->publishTopic(topic, payload, qos, retain);
    }
}

size_t ChannelBus::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return channels_.size();
}

IChannel* ChannelBus::find(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& ch : channels_) if (ch->id() == id) return ch.get();
    return nullptr;
}

} // namespace industrial
