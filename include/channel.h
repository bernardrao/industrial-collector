#pragma once
/**
 * channel.h — 上行通道抽象
 *
 * P3.1 修 MQTT 上行 drop 的过程钉出了通道语义的三条硬约束：
 *
 *   ── 批量成帧      —— 一万多点逐条走客户端队列会打爆缓冲，必须分片
 *   ── 分片编号      —— 下游用 timestamp + 序号才能重装一整轮
 *   ── 全轮共用一个 ts —— 分片各自算 nowIso() 的话，同一轮的分片时间戳互不相同，
 *                        下游按 ts 分桶就永远拼不出完整一轮（P3.1 实测踩到）
 *
 * 这三条对 MQTT / Kafka / 文件三种通道**都成立**，所以放进接口。避免每加一种
 * 通道就重新论证一次"能不能重装"。
 *
 * ── 接口原则 ────────────────────────────────────────────────────────────────
 *   publishRound() 拿的是【一整轮 + 一个 ts】，通道自己决定要不要分片。
 *   qos / retain / 主题前缀这类**通道特有**的参数不进接口，由通道从自己的
 *   config 里取。以前采集器把 qos 从 `state.config.mqtt` 里读出来往下传是
 *   历史遗留 —— 现在 MQTT 不再是唯一通道，那样的耦合就守不住了。
 *
 * ── 与 P3.1 分片方案的兼容性 ────────────────────────────────────────────────
 *   MqttChannel 继续按 P3.1 的形状发：小设备逐点 + prefix/batch；大规格设备
 *   走 prefix/batch/000..NNN。下游订阅一行不改。
 */
#include "types.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace industrial {

class IChannel {
public:
    virtual ~IChannel() = default;

    // 连接（对文件通道就是 open；对 MQTT / Kafka 是建立会话）。失败抛异常，
    // 由 ChannelBus 决定是禁用该通道还是拒绝启动。
    virtual void connect()    = 0;
    virtual void disconnect() = 0;

    // 是否可发。ChannelBus 按此跳过掉线的通道 —— 但通道自己应处理短暂断连
    // （重连 / 缓冲），这里主要是给"配置错误 / 认证失败"这类持续性问题。
    virtual bool ready() const = 0;

    // 一整轮数据 → 通道。ts 是"这一轮的墙钟时间"，通道内所有帧共用它。
    // device_id 用作路由键（MQTT 拼 topic、文件当文件名、Kafka 当 key）。
    // qos / retain 不进接口：那是 MQTT 的概念，Kafka / 文件不认。
    virtual void publishRound(const std::string& device_id,
                               const DataPoints& points,
                               const std::string& ts) = 0;

    // 用于日志与调试。id = channels 表里的 id；type ∈ {mqtt, kafka, file}
    virtual const std::string& id()   const = 0;
    virtual const std::string& type() const = 0;
};

// 通道总线：一次采集轮 → 所有启用的上行通道。
//
// 目前串行分发。ESS 站实测 P3.1 修完后 MQTT 一轮 ~几十 ms，多个通道串行也够
// 用；真出现慢通道拖累采集的场景（Kafka 到远端集群），单独做一层内部队列。
// 不要为设想中的性能问题预置结构 —— 先跑起来看真实数据。
class ChannelBus {
public:
    ChannelBus() = default;
    ~ChannelBus();

    void add(std::unique_ptr<IChannel> ch);   // ctor 后到 startAll() 前调用
    void startAll();                          // 所有通道 connect()；单个失败不阻塞其余
    void stopAll();

    // 原子替换整套通道 —— 通道表改完了给 main 用的入口：新表下发、老通道断开、
    // 新通道 connect。中间 publishRound 会等一小会儿（毫秒级），值得为"改配置
    // 不重启进程"这条路。after_start 由 main 提供，负责把缓存挂回主 MQTT 通道、
    // 重注册下行指令回调 —— 那些都需要 main 里的上下文（cache/state/cfgdb）。
    void reset(std::vector<std::unique_ptr<IChannel>> newChs,
               std::function<void(ChannelBus&)> after_start = nullptr);

    // 一整轮 → 每个 ready() 的通道各发一遍。
    // ts 在这里【生成一次】，所有通道共用同一个墙钟时刻 —— 上层保持简单。
    void publishRound(const std::string& device_id, const DataPoints& points);

    // 单点主题发布（供 MqttChannel 内部续传 / 指令回执用；文件通道当追加一行）。
    // 保留是因为老的续传路径直接调 publish(topic,payload,qos,retain)。
    // 只对 MQTT 有意义，其余通道会忽略。
    // 迁移期方便，等 P3.2b 下行指令抽出来后可以撤掉。
    void publishTopic(const std::string& topic, const std::string& payload,
                       int qos, bool retain);

    size_t size() const;
    IChannel* find(const std::string& id) const;   // 用于给主 MQTT 通道挂缓存

private:
    // publishRound 走"读"（多线程并发），reset 走"写"（互斥）。改成 shared_mutex
    // 也可以，但反正 reset 是人手点一下的低频动作，简单 mutex 够了。
    mutable std::mutex                     mtx_;
    std::vector<std::unique_ptr<IChannel>> channels_;
};

// ── 具体通道 ────────────────────────────────────────────────────────────────

class DataCache;
struct CacheConfig;

// MQTT 通道：整轮分片按 P3.1 的形状发（小设备逐点 + prefix/batch；大规格
// 走 prefix/batch/000..NNN，全轮共用一个 ts）。
struct MqttChannelConfig {
    std::string id;
    std::string broker;
    int         port      = 1883;
    std::string client_id;
    std::string username;
    std::string password;
    std::string topic_prefix;
    int         qos       = 1;
    bool        retain    = false;
    int         keepalive = 60;
};

class MqttChannel : public IChannel {
public:
    explicit MqttChannel(const MqttChannelConfig& cfg);
    ~MqttChannel() override;

    void connect() override;
    void disconnect() override;
    bool ready() const override;

    void publishRound(const std::string& device_id, const DataPoints& points,
                       const std::string& ts) override;

    const std::string& id()   const override { return id_; }
    const std::string& type() const override;

    // 单点主题（供续传 / 指令回执用）
    void publishTopic(const std::string& topic, const std::string& payload,
                       int qos, bool retain);

    // 订阅一个下行指令主题（带通配符），命中的消息回调给 handler。
    // 用途：任务级启停 `<prefix>/cmd/task/<id>/{start,stop}`。
    // 每个 MQTT 通道各自订自己 topic_prefix 下的指令 —— 换句话说下行走谁上行也
    // 走谁，两边成对。多 MQTT 通道时 handler 会被各自触发一次。
    using CmdHandler = std::function<void(const std::string& topic,
                                          const std::string& payload)>;
    void subscribeCmd(const std::string& topic_filter, CmdHandler handler);

    // 挂本地缓存：只有 MQTT 通道有意义（续传主题、指令订阅）。
    // 简化：整个进程只允许一个 MQTT 通道挂缓存 —— 多路续传的语义还没定，
    // 硬做只会又踩坑。ChannelBus 会自动挑第一个 mqtt 通道挂上去。
    void attachCache(DataCache* cache, const CacheConfig& cc);

    // 由内部回调触发（旧 MqttPublisher 保留下来的两个入口）
    void onConnected();
    void onCommand(const std::string& topic, const std::string& payload);

private:
    struct Impl;
    std::string           id_;    // id() 返回引用，成员先声明 pimpl 后建
    std::unique_ptr<Impl> pimpl_;
};

// 文件通道：JSON-Lines，每行一整轮。
//   {ts, device_id, points: {name: {value,quality,unit}, ...}}
// 用途：气隙场景把数据摆到磁盘让别的进程/穿网闸摆渡；离线诊断时录一段回放。
// 每个 device_id 一份文件（`<dir>/<device_id>.jsonl`），追加写入。
struct FileChannelConfig {
    std::string id;
    std::string dir;             // 目标目录，不存在会自动建
    int         rotate_mb = 0;   // 0 = 不轮转；> 0 = 单文件超过 N MB 时换 .1 .2 ...
    int         keep      = 5;   // 轮转保留份数
};

class FileChannel : public IChannel {
public:
    struct Impl;
    explicit FileChannel(const FileChannelConfig& cfg);
    ~FileChannel() override;

    void connect() override;
    void disconnect() override;
    bool ready() const override;

    void publishRound(const std::string& device_id, const DataPoints& points,
                       const std::string& ts) override;

    const std::string& id()   const override { return cfg_.id; }
    const std::string& type() const override;

private:
    FileChannelConfig     cfg_;   // 先声明：id() 的返回引用要它先在，pimpl 靠后
    std::unique_ptr<Impl> pimpl_;
};

} // namespace industrial
