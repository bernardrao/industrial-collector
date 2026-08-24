#pragma once
/**
 * iec61850_collector.h — IEC 61850 MMS 客户端采集器
 *
 * 基于 libiec61850 (https://github.com/mz-automation/libiec61850)
 * 静态链接，Ubuntu 18 兼容
 *
 * 支持:
 *   - MMS Read  (轮询模式，ReadRequest 读取 DA 值)
 *   - BRCB 缓冲报告订阅 (use_reports=true，事件驱动，低延迟)
 *   - 数据类型: BOOLEAN / INT8~INT64 / UINT8~UINT64 / FLOAT32 / FLOAT64
 *               VISIBLE_STRING / BIT_STRING / UTC_TIME
 *
 * IEC61850 对象寻址示例:
 *   object_ref = "MEAS/MMXU1.PhV.phsA.cVal.mag.f"
 *   fc         = "MX"   (实测值)
 *
 * 常用 FC（功能约束）:
 *   MX - 测量值        ST - 状态值
 *   CO - 可控          CF - 配置
 *   DC - 描述          EX - 扩展定义
 */

#include "types.h"
#include "config.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <string>

// 前向声明 libiec61850 C 类型，避免污染头文件
struct sIedConnection;
struct sMmsValue;

namespace industrial {

// 缓存的 MMS 值（报告模式下由回调写入）
struct MmsCache {
    double      value   = 0.0;
    Quality     quality = Quality::BAD;
    std::string timestamp;
};

class IEC61850Collector {
public:
    explicit IEC61850Collector(const IEC61850Config& cfg);
    ~IEC61850Collector();

    void       connect();
    void       disconnect();
    DataPoints readAll();

    double pollInterval() const { return cfg_.poll_interval; }
    bool   isConnected()  const { return connected_; }

private:
    IEC61850Config cfg_;
    sIedConnection* conn_  = nullptr;
    bool            connected_ = false;

    // 报告模式相关
    mutable std::mutex                          cacheMtx_;
    std::unordered_map<std::string, MmsCache>   cache_;   // object_ref -> 缓存值
    std::atomic<bool>                           running_{false};

    // 轮询读取单个数据属性
    DataPoint readPoint(const IEC61850Point& pt);

    // MMS 值解码
    static double     decodeMmsValue(sMmsValue* val);
    static Quality    decodeQuality(sMmsValue* qualVal);
    static double     applyScale(double raw, float scale, float offset);

    // 报告回调（静态，通过 userData 转回 this）
    static void reportCallback(void* userData, int reason,
                                const char* objRef, sMmsValue* val);

    void enableReports();
    void disableReports();
};

} // namespace industrial
