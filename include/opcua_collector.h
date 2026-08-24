#pragma once
/**
 * opcua_collector.h — OPC UA 客户端采集器
 *
 * 基于 open62541 v1.3 (https://github.com/open62541/open62541)
 * 单文件 amalgamation 编译，静态链接，Ubuntu 18 兼容
 *
 * 支持:
 *   - UA_Client_readValueAttribute  (同步轮询读取节点值)
 *   - 订阅 + MonitoredItem          (事件驱动，低延迟，可选)
 *   - 安全策略: None（内网场景）
 *   - 数据类型: Boolean / SByte ~ Int64 / UByte ~ UInt64
 *               Float / Double / String / DateTime
 *
 * NodeId 格式示例:
 *   "ns=2;i=1001"        数字型
 *   "ns=2;s=Temperature" 字符串型
 *   "ns=1;g=..."         GUID 型（较少见）
 */

#include "types.h"
#include "config.h"
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

// 前向声明 open62541 核心类型
struct UA_Client;

namespace industrial {

class OpcUaCollector {
public:
    explicit OpcUaCollector(const OpcUaConfig& cfg);
    ~OpcUaCollector();

    void       connect();
    void       disconnect();
    DataPoints readAll();

    double pollInterval() const { return cfg_.poll_interval; }
    bool   isConnected()  const { return connected_; }

private:
    OpcUaConfig cfg_;
    UA_Client*  client_    = nullptr;
    bool        connected_ = false;

    DataPoint readPoint(const OpcUaPoint& pt);

    // NodeId 字符串解析: "ns=2;i=1001" / "ns=2;s=Name"
    // 返回 open62541 UA_NodeId（内部使用 void* 避免暴露 open62541 类型）
    static bool parseNodeId(const std::string& str,
                             uint16_t& ns, uint32_t& numericId,
                             std::string& stringId, bool& isNumeric);

    static double  decodeVariant(void* variant);   // UA_Variant*
    static Quality statusToQuality(uint32_t statusCode);
    static double  applyScale(double raw, float scale, float offset);
};

} // namespace industrial
