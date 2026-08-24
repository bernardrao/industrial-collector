// src/opcua_collector.cpp — OPC UA 客户端实现
// 基于 open62541 v1.3 amalgamation

#include "opcua_collector.h"
#include <spdlog/spdlog.h>

// open62541 单文件 amalgamation
// 必须在 .cpp 中 include，不能放在头文件
#include <open62541/open62541.h>

#include <stdexcept>
#include <cstring>
#include <sstream>

namespace industrial {

// ── NodeId 解析: "ns=2;i=1001" / "ns=2;s=Temperature" ────────────────────────
bool OpcUaCollector::parseNodeId(const std::string& str,
                                  uint16_t& ns,
                                  uint32_t& numericId,
                                  std::string& stringId,
                                  bool& isNumeric)
{
    // 格式: ns=<uint>;i=<uint>   数字型
    //       ns=<uint>;s=<str>    字符串型
    ns        = 0;
    numericId = 0;
    stringId.clear();
    isNumeric = false;

    // 解析 ns=
    size_t nsPos = str.find("ns=");
    if (nsPos == std::string::npos) {
        // 无 ns= 前缀，默认 ns=0
        ns = 0;
    } else {
        size_t semi = str.find(';', nsPos);
        if (semi == std::string::npos) return false;
        try { ns = static_cast<uint16_t>(
                std::stoul(str.substr(nsPos + 3, semi - nsPos - 3))); }
        catch (...) { return false; }
    }

    // 解析 i= 或 s=
    size_t iPos = str.find(";i=");
    size_t sPos = str.find(";s=");

    if (iPos != std::string::npos) {
        isNumeric = true;
        try { numericId = std::stoul(str.substr(iPos + 3)); }
        catch (...) { return false; }
    } else if (sPos != std::string::npos) {
        isNumeric = false;
        stringId  = str.substr(sPos + 3);
    } else {
        // 尝试不带 ns= 前缀的纯数字
        try { numericId = std::stoul(str); isNumeric = true; }
        catch (...) { return false; }
    }
    return true;
}

// ── UA_Variant → double ────────────────────────────────────────────────────────
double OpcUaCollector::decodeVariant(void* vptr) {
    if (!vptr) return 0.0;
    UA_Variant* v = static_cast<UA_Variant*>(vptr);
    if (UA_Variant_isEmpty(v)) return 0.0;

    const UA_DataType* t = v->type;
    void* d = v->data;

    if (t == &UA_TYPES[UA_TYPES_BOOLEAN])
        return *static_cast<UA_Boolean*>(d) ? 1.0 : 0.0;
    if (t == &UA_TYPES[UA_TYPES_SBYTE])
        return static_cast<double>(*static_cast<UA_SByte*>(d));
    if (t == &UA_TYPES[UA_TYPES_BYTE])
        return static_cast<double>(*static_cast<UA_Byte*>(d));
    if (t == &UA_TYPES[UA_TYPES_INT16])
        return static_cast<double>(*static_cast<UA_Int16*>(d));
    if (t == &UA_TYPES[UA_TYPES_UINT16])
        return static_cast<double>(*static_cast<UA_UInt16*>(d));
    if (t == &UA_TYPES[UA_TYPES_INT32])
        return static_cast<double>(*static_cast<UA_Int32*>(d));
    if (t == &UA_TYPES[UA_TYPES_UINT32])
        return static_cast<double>(*static_cast<UA_UInt32*>(d));
    if (t == &UA_TYPES[UA_TYPES_INT64])
        return static_cast<double>(*static_cast<UA_Int64*>(d));
    if (t == &UA_TYPES[UA_TYPES_UINT64])
        return static_cast<double>(*static_cast<UA_UInt64*>(d));
    if (t == &UA_TYPES[UA_TYPES_FLOAT])
        return static_cast<double>(*static_cast<UA_Float*>(d));
    if (t == &UA_TYPES[UA_TYPES_DOUBLE])
        return *static_cast<UA_Double*>(d);
    if (t == &UA_TYPES[UA_TYPES_STRING]) {
        UA_String* s = static_cast<UA_String*>(d);
        if (s->length > 0 && s->data) {
            std::string str(reinterpret_cast<char*>(s->data), s->length);
            try { return std::stod(str); } catch (...) {}
        }
        return 0.0;
    }
    if (t == &UA_TYPES[UA_TYPES_DATETIME]) {
        // DateTime 是 100ns 间隔从 1601-01-01，转为 Unix 时间戳
        UA_DateTime dt = *static_cast<UA_DateTime*>(d);
        // 1601→1970 差值: 11644473600 秒
        return static_cast<double>(dt / 10000000LL) - 11644473600.0;
    }
    spdlog::debug("OPC-UA 未知数据类型: {}", t->typeName);
    return 0.0;
}

// ── StatusCode → Quality ───────────────────────────────────────────────────────
Quality OpcUaCollector::statusToQuality(uint32_t code) {
    // OPC-UA StatusCode: 高2位 00=Good 01=Uncertain 10/11=Bad
    uint32_t severity = (code >> 30) & 0x03;
    if (severity == 0) return Quality::GOOD;
    if (severity == 1) return Quality::UNCERTAIN;
    return Quality::BAD;
}

double OpcUaCollector::applyScale(double raw, float scale, float offset) {
    return raw * scale + offset;
}

// ── 构造 / 析构 ───────────────────────────────────────────────────────────────
OpcUaCollector::OpcUaCollector(const OpcUaConfig& cfg) : cfg_(cfg) {}

OpcUaCollector::~OpcUaCollector() { disconnect(); }

// ── 连接 ─────────────────────────────────────────────────────────────────────
void OpcUaCollector::connect() {
    client_ = UA_Client_new();
    if (!client_) throw std::runtime_error("UA_Client_new 失败");

    UA_ClientConfig* config = UA_Client_getConfig(client_);
    UA_ClientConfig_setDefault(config);

    // 设置超时（毫秒）
    config->timeout = static_cast<UA_UInt32>(cfg_.timeout_ms);

    UA_StatusCode sc;

    // 根据是否有用户名选择连接方式
    if (!cfg_.username.empty()) {
        sc = UA_Client_connectUsername(
            client_,
            cfg_.endpoint_url.c_str(),
            cfg_.username.c_str(),
            cfg_.password.c_str());
    } else {
        sc = UA_Client_connect(client_, cfg_.endpoint_url.c_str());
    }

    if (sc != UA_STATUSCODE_GOOD) {
        UA_Client_delete(client_);
        client_ = nullptr;
        std::ostringstream oss;
        oss << "OPC-UA 连接失败 " << cfg_.endpoint_url
            << " StatusCode=0x" << std::hex << sc
            << " (" << UA_StatusCode_name(sc) << ")";
        throw std::runtime_error(oss.str());
    }

    connected_ = true;
    spdlog::info("OPC-UA 已连接 {}", cfg_.endpoint_url);
}

// ── 断开 ─────────────────────────────────────────────────────────────────────
void OpcUaCollector::disconnect() {
    if (client_) {
        UA_Client_disconnect(client_);
        UA_Client_delete(client_);
        client_    = nullptr;
        connected_ = false;
        spdlog::info("OPC-UA 已断开");
    }
}

// ── 读所有点 ─────────────────────────────────────────────────────────────────
DataPoints OpcUaCollector::readAll() {
    DataPoints result;
    result.reserve(cfg_.points.size());

    for (const auto& pt : cfg_.points) {
        result.push_back(readPoint(pt));
    }

    // 保活：run iterate 防止服务器超时断开
    if (client_ && connected_) {
        UA_Client_run_iterate(client_, 0);
    }

    int good = 0;
    for (auto& p : result) if (p.quality == Quality::GOOD) ++good;
    spdlog::info("OPC-UA 采集: {}/{} 点 GOOD", good, result.size());
    return result;
}

// ── 读单个节点 ────────────────────────────────────────────────────────────────
DataPoint OpcUaCollector::readPoint(const OpcUaPoint& pt) {
    DataPoint dp;
    dp.name        = pt.name;
    dp.unit        = pt.unit;
    dp.description = pt.description;
    dp.timestamp   = nowIso();
    dp.quality     = Quality::BAD;

    if (!connected_ || !client_) return dp;

    // 解析 NodeId 字符串
    uint16_t    ns = 0;
    uint32_t    numId = 0;
    std::string strId;
    bool        isNum = false;

    if (!parseNodeId(pt.node_id, ns, numId, strId, isNum)) {
        spdlog::error("OPC-UA NodeId 解析失败: {}", pt.node_id);
        return dp;
    }

    // 构造 UA_NodeId
    UA_NodeId nodeId;
    if (isNum) {
        nodeId = UA_NODEID_NUMERIC(ns, numId);
    } else {
        nodeId = UA_NODEID_STRING_ALLOC(ns, strId.c_str());
    }

    // 读取值
    UA_Variant value;
    UA_Variant_init(&value);

    UA_StatusCode sc = UA_Client_readValueAttribute(client_, nodeId, &value);

    if (!isNum) UA_NodeId_clear(&nodeId);

    if (sc != UA_STATUSCODE_GOOD) {
        spdlog::warn("OPC-UA 读取 {} ({}) 失败: 0x{:X} {}",
                     pt.name, pt.node_id, sc, UA_StatusCode_name(sc));
        UA_Variant_clear(&value);

        // 尝试重连
        UA_Client_disconnect(client_);
        UA_StatusCode rsc = cfg_.username.empty()
            ? UA_Client_connect(client_, cfg_.endpoint_url.c_str())
            : UA_Client_connectUsername(client_,
                cfg_.endpoint_url.c_str(),
                cfg_.username.c_str(),
                cfg_.password.c_str());
        connected_ = (rsc == UA_STATUSCODE_GOOD);
        if (connected_) spdlog::info("OPC-UA 重连成功");
        return dp;
    }

    double raw = decodeVariant(&value);
    UA_Variant_clear(&value);

    dp.raw_value = static_cast<float>(raw);
    dp.value     = static_cast<float>(applyScale(raw, pt.scale, pt.offset));
    dp.quality   = Quality::GOOD;

    spdlog::debug("  OPC-UA {} = {} {}", pt.name,
                  dp.toDouble().value_or(0.0), pt.unit);
    return dp;
}

} // namespace industrial
