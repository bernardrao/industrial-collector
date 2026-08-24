#pragma once
/**
 * mqtt_publisher.h — Paho MQTT C++ 异步发布器 + 本地缓存/断点续传
 */
#include "types.h"
#include "config.h"
#include "data_cache.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mqtt { class async_client; class connect_options; class callback; }

namespace industrial {

class MqttPublisher {
public:
    explicit MqttPublisher(const MqttConfig& cfg);
    ~MqttPublisher();

    // 在 connect() 之前调用：挂接本地缓存与续传配置（cache 可为 nullptr=禁用）
    void attachCache(DataCache* cache, const CacheConfig& cc);

    void connect();
    void disconnect();
    void publish(const std::string& topic, const std::string& payload,
                 int qos = 1, bool retain = false);
    // 实时发布 + 无条件写入本地缓存（device_id=prefix）
    // publish_mode="batch"（默认）: 单点主题 + /batch 主题
    // publish_mode="array"        : 每个 range 一条数组消息，范围点不发单点主题
    void publishPoints(const DataPoints& points,
                       const std::string& prefix, int qos, bool retain,
                       const std::string& publish_mode = "batch",
                       const std::vector<ModbusRange>& ranges = {});

    bool isConnected() const { return connected_.load(); }

    // 订阅一个额外的指令主题（带通配符），把命中的消息回调给上层。
    // 存量的续传主题（cache.command_topic）仍由 MqttPublisher 自己接管，这里加
    // 的是"任务级下行控制"这类新入口 —— 主题模式 + 处理函数由 main 提供，
    // 免得 MqttPublisher 长出一堆业务分支。
    // connect() 之前调用；重连时会自动重订阅（clean_session）。
    using CmdHandler = std::function<void(const std::string& topic,
                                          const std::string& payload)>;
    void subscribeCmd(const std::string& topic_filter, CmdHandler handler);

    // ── 由 MqttCb 回调触发 ──
    void onConnected();                                   // (重)订阅指令主题
    void onCommand(const std::string& topic, const std::string& payload);

private:
    MqttConfig  cfg_;
    DataCache*  cache_ = nullptr;
    CacheConfig cacheCfg_;
    bool        cacheOn_ = false;

    std::unique_ptr<mqtt::async_client>    client_;
    std::unique_ptr<mqtt::connect_options> connOpts_;
    std::shared_ptr<mqtt::callback>        cb_;   // 回调对象，生命周期随发布器
    std::atomic<bool> connected_{false};

    // ── 回放线程 ──
    enum class Mode { RESUME, SINCE, RANGE };
    struct Req { Mode mode; int64_t since; std::string start, end; };
    std::thread             replayThread_;
    std::mutex              reqMtx_;
    std::condition_variable reqCv_;
    std::deque<Req>         reqQ_;
    std::atomic<bool>       stop_{false};

    // ── 发布失败的聚合上报 ──
    // 一个储能站一轮要发一万多条，broker 慢一点就整轮失败。逐条打日志的话，
    // 一个周期能写出近万行 —— 轮转日志几秒就被填满，Web 那 500 条的日志环也
    // 被冲光，真正有用的信息全被挤掉。故只即时报第一条，其余按秒聚合。
    std::mutex                            failMtx_;
    uint64_t                              failCount_ = 0;
    std::string                           failLastTopic_, failLastWhat_;
    std::chrono::steady_clock::time_point failLastReport_{};
    void noteFailure(const std::string& topic, const std::string& what);

    // 外部注册的下行指令订阅（任务启停等）。存的是 (topic_filter, handler)。
    struct CmdSub { std::string filter; CmdHandler handler; };
    std::vector<CmdSub> cmdSubs_;
    static bool topicMatch(const std::string& filter, const std::string& topic);

    void replayLoop();
    void doReplay(const Req& req);

    static std::string buildPointJson(const DataPoint& p);
    static std::string buildBatchJson(const DataPoints& pts, const std::string& ts);
};

} // namespace industrial
