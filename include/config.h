#pragma once
/**
 * config.h — 多设备混合协议配置结构体
 *
 * 配置文件顶层结构：
 *   { "mqtt":{...}, "web":{...}, "logging":{...}, "devices":[...] }
 *
 * 每个 device 条目：
 *   { "id":"xxx", "protocol":"modbus|iec104|iec61850|opcua|dlt645|dlt698|can",
 *     "modbus":{...}  // 只填对应协议的块
 *   }
 *
 * DLT645/698 采用"总线"模型：一个条目 = 一个物理端点（串口或TCP网关），
 *   内含 meters[] 数组；同一端点的表由同一线程顺序轮询。
 *
 * CAN：protocol "canfd" 是 "can" + can.fd=true 的简写，两者共用 can 配置块。
 */
#include "types.h"
#include <string>
#include <vector>

namespace industrial {

// ── MQTT ─────────────────────────────────────────────────────────────────────
struct MqttConfig {
    std::string broker       = "localhost";
    int         port         = 1883;
    std::string client_id    = "industrial_collector";
    std::string username;
    std::string password;
    std::string topic_prefix = "factory/line1";
    int         qos          = 1;
    bool        retain       = false;
    int         keepalive    = 60;
};

// ── Modbus TCP ───────────────────────────────────────────────────────────────
struct ModbusConfig {
    std::string              host          = "127.0.0.1";
    int                      port          = 502;
    int                      unit_id       = 1;
    int                      timeout_sec   = 3;
    double                   poll_interval = 1.0;
    // publish_mode: "batch"（默认，逐点+batch 主题）
    //               "array"（大规模：每个 range 一条数组消息，范围点不发单点主题）
    std::string              publish_mode  = "batch";
    std::vector<ModbusRange> ranges;    // 范围展开（运行时展开后追加到 points）
    std::vector<ModbusPoint> points;    // 手配命名点 + 展开后的 range 点（from_range=true）
};

// ── IEC 60870-5-104 ──────────────────────────────────────────────────────────
struct IEC104Config {
    std::string              host           = "127.0.0.1";
    int                      port           = 2404;
    int                      common_address = 1;
    double                   poll_interval  = 2.0;
    int                      timeout_sec    = 10;
    std::vector<IEC104Point> points;
};

// ── IEC 61850 MMS ────────────────────────────────────────────────────────────
struct IEC61850Config {
    std::string                host           = "127.0.0.1";
    int                        port           = 102;
    double                     poll_interval  = 1.0;
    int                        timeout_ms     = 5000;
    std::string                auth_password;
    bool                       use_reports    = false;
    std::vector<IEC61850Point> points;
};

// ── OPC UA ───────────────────────────────────────────────────────────────────
struct OpcUaConfig {
    std::string             endpoint_url    = "opc.tcp://127.0.0.1:4840";
    double                  poll_interval   = 1.0;
    int                     timeout_ms      = 5000;
    std::string             security_policy = "None";
    std::string             username;
    std::string             password;
    std::vector<OpcUaPoint> points;
};

// ── DLT645 总线配置 ──────────────────────────────────────────────────────────
// 一个条目 = 一根串口（或一个 TCP 网关），可挂多块表。
// 同一端点下的表由同一线程顺序轮询，保证 RS485 帧不冲突。
struct BusDlt645Config {
    std::string connection_type = "serial";   // "serial" | "tcp"
    std::string serial_port     = "/dev/ttyUSB0";
    int         baud_rate       = 9600;
    std::string host;                         // TCP 网关
    int         tcp_port        = 20108;
    double      poll_interval   = 5.0;        // 对整条总线的一轮的最小间隔（秒）
    int         timeout_ms      = 3000;       // 单表单次请求超时
    std::vector<MeterConfig645> meters;
};

// ── DLT698 总线配置 ──────────────────────────────────────────────────────────
struct BusDlt698Config {
    std::string connection_type = "serial";
    std::string serial_port     = "/dev/ttyUSB0";
    int         baud_rate       = 9600;
    std::string host;
    int         tcp_port        = 20108;
    double      poll_interval   = 5.0;
    int         timeout_ms      = 5000;
    std::vector<MeterConfig698> meters;
};

// ── CAN / CAN FD（SocketCAN）─────────────────────────────────────────────────
// 被动监听模型：后台线程持续收帧，按 CAN ID 只保留最新一帧；每个 poll_interval
// 从该缓存解码全部信号。距上次收到超过 stale_timeout_ms 的帧，其信号标记为 BAD。
//
// 注意：RAW socket 无法设置比特率，接口必须已由系统拉起，例如
//   ip link set can0 up type can bitrate 500000                    (经典 CAN)
//   ip link set can0 up type can bitrate 500000 dbitrate 2000000 fd on   (CAN FD)
// 因此这里没有 bitrate 配置项——写了也不会生效。
struct CanConfig {
    std::string interface        = "can0";
    bool        fd               = false;   // true = 启用 CAN FD（负载最长 64 字节）
    double      poll_interval    = 1.0;     // 解码/上报周期（秒）
    int         stale_timeout_ms = 5000;    // 帧有效期；<=0 表示永不过期
    std::vector<CanSignal> signals;
};

// ── Web / Log ────────────────────────────────────────────────────────────────
struct WebConfig {
    int         port    = 8080;
    std::string bind    = "0.0.0.0";
    bool        enabled = true;
    // debug 模式：从磁盘 web/ 目录实时伺服前端（改完刷新即可，免重新编译）。
    // 也可用命令行 --debug 开启。debug_root 空时用编译期 IC_WEB_SOURCE_DIR。
    bool        debug      = false;
    std::string debug_root;
    // HTTPS：tls_enabled=true 时用 SSLServer，需 cert/key（PEM）。
    // 生成自签证书：bash scripts/gen_cert.sh
    bool        tls_enabled = false;
    std::string tls_cert    = "cert.pem";
    std::string tls_key     = "key.pem";

    // HTTP Basic 鉴权。开启后【所有】路由（静态页 + /api/*）都需要认证——
    // /api/control 能停机、/api/config/full 能改写配置，仅保护写接口是不够的。
    // 注意 TLS 关闭时 Basic 凭据在网络上等同明文，公网暴露务必同时开 tls_enabled。
    // auth_password 明文存于 config.json（与 mqtt.password 一致），建议 chmod 600。
    bool        auth_enabled  = false;
    std::string auth_user     = "admin";
    std::string auth_password;
};

struct LogConfig {
    std::string level        = "info";
    std::string file         = "collector.log";
    size_t      max_bytes    = 10 * 1024 * 1024;
    int         backup_count = 5;
};

// ── 本地缓存 / 断点续传 ──────────────────────────────────────────────────────
// 无论 MQTT 是否在线都缓存；收到续传指令后回放到 history_topic。
//   指令 JSON（发到 command_topic）：
//     {}                              普通续传：从持久化断点继续，发完推进断点
//     {"since": <rowid>}              从指定游标续传（不动断点）
//     {"start":"ISO","end":"ISO"}     按时间段回放（不动断点）
struct CacheConfig {
    bool        enabled        = true;
    std::string db_path        = "cache.db";
    int         retention_hours= 24;          // 缓存保留时长
    int64_t     max_rows       = 1000000;     // 行数上限（防磁盘失控）
    std::string history_topic;                // 空=用 {prefix}/history
    std::string command_topic;                // 空=用 {prefix}/cmd/replay
    int         replay_rate_ms = 50;          // 每条回放间隔
    int         replay_chunk   = 200;         // 每次查询块大小
};

// ── 单设备条目 ───────────────────────────────────────────────────────────────
struct DeviceEntry {
    std::string    id;
    bool           enabled  = true;
    Protocol       protocol = Protocol::MODBUS;

    // 断线后重连间隔（秒）。每个设备独立线程内无限重连，间隔由用户配置。
    int            reconnect_interval_sec = 5;

    // 只有与 protocol 匹配的块会被使用
    ModbusConfig    modbus;
    IEC104Config    iec104;
    IEC61850Config  iec61850;
    OpcUaConfig     opcua;
    BusDlt645Config dlt645;
    BusDlt698Config dlt698;
    CanConfig       can;

    // 当前协议的轮询周期（便于上层统一读取）
    double pollInterval() const {
        switch (protocol) {
            case Protocol::MODBUS:   return modbus.poll_interval;
            case Protocol::IEC104:   return iec104.poll_interval;
            case Protocol::IEC61850: return iec61850.poll_interval;
            case Protocol::OPCUA:    return opcua.poll_interval;
            case Protocol::DLT645:   return dlt645.poll_interval;
            case Protocol::DLT698:   return dlt698.poll_interval;
            case Protocol::CAN:      return can.poll_interval;
        }
        return 1.0;
    }
};

// ── 全局配置 ─────────────────────────────────────────────────────────────────
struct AppConfig {
    MqttConfig               mqtt;
    std::vector<DeviceEntry> devices;
    WebConfig                web;
    LogConfig                logging;
    CacheConfig              cache;
};

AppConfig   loadConfig(const std::string& path);
void        saveConfig(const AppConfig& cfg, const std::string& path);

// Web 配置 API 用：内存序列化/反序列化（无文件 I/O）
std::string appConfigToJsonString(const AppConfig& cfg);
AppConfig   appConfigFromJsonString(const std::string& jsonStr);

} // namespace industrial
