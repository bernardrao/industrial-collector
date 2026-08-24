#pragma once
/**
 * types.h — 公共数据结构
 * 支持协议: Modbus / IEC104 / IEC61850 / OPC-UA / DLT645 / DLT698 / CAN(FD)
 */
#include <string>
#include <variant>
#include <vector>
#include <optional>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>

namespace industrial {

// ── 质量标志 ───────────────────────────────────────────────────────────────────
enum class Quality : uint8_t { GOOD = 0, BAD = 1, UNCERTAIN = 2 };
inline const char* qualityStr(Quality q) {
    switch (q) {
        case Quality::GOOD:      return "GOOD";
        case Quality::BAD:       return "BAD";
        case Quality::UNCERTAIN: return "UNCERTAIN";
    }
    return "UNKNOWN";
}

// ── 点值 ───────────────────────────────────────────────────────────────────────
using PointValue = std::variant<std::monostate, bool, int32_t, uint32_t, float, double>;

// ── 单个采集点 ──────────────────────────────────────────────────────────────────
struct DataPoint {
    std::string name;
    PointValue  value;
    Quality     quality     = Quality::BAD;
    std::string timestamp;
    std::string unit;
    std::string description;
    PointValue  raw_value;
    bool        from_range  = false;   // 由 ModbusRange 展开，array 模式用于路由

    std::optional<double> toDouble() const {
        return std::visit([](auto&& v) -> std::optional<double> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) return std::nullopt;
            else return static_cast<double>(v);
        }, value);
    }
};
using DataPoints = std::vector<DataPoint>;

// ── 协议选择 ───────────────────────────────────────────────────────────────────
enum class Protocol { MODBUS, IEC104, IEC61850, OPCUA, DLT645, DLT698, CAN };

inline const char* protocolStr(Protocol p) {
    switch (p) {
        case Protocol::MODBUS:   return "MODBUS";
        case Protocol::IEC104:   return "IEC104";
        case Protocol::IEC61850: return "IEC61850";
        case Protocol::OPCUA:    return "OPCUA";
        case Protocol::DLT645:   return "DLT645";
        case Protocol::DLT698:   return "DLT698";
        case Protocol::CAN:      return "CAN";
    }
    return "UNKNOWN";
}

// ── Modbus 配置 ─────────────────────────────────────────────────────────────────
enum class ModbusDataType { BOOL, INT16, UINT16, INT32, UINT32, FLOAT32 };
struct ModbusPoint {
    std::string    name;
    int            address   = 0;
    int            func_code = 3;
    ModbusDataType data_type = ModbusDataType::UINT16;
    float          scale     = 1.0f;
    float          offset    = 0.0f;
    std::string    unit;
    std::string    description;
    bool           from_range = false;   // true = 由 ranges[] 展开，序列化时跳过
};

// ── Modbus 范围展开（大规模场景：一行配置生成 N 个同构测点）──────────────────────
struct ModbusRange {
    std::string    name_prefix;          // 点名前缀，展开为 {prefix}0000~{prefix}N-1
    int            addr_start  = 0;      // 起始寄存器地址
    int            count       = 0;      // 展开点数
    int            func_code   = 3;
    ModbusDataType data_type   = ModbusDataType::UINT16;
    float          scale       = 1.0f;
    float          offset      = 0.0f;
    std::string    unit;
    std::string    description;
};

// ── IEC104 配置 ─────────────────────────────────────────────────────────────────
struct IEC104Point {
    std::string name;
    int         ioa     = 0;
    int         type_id = 13;
    float       scale   = 1.0f;
    float       offset  = 0.0f;
    std::string unit;
    std::string description;
};

// ── IEC61850 配置 ───────────────────────────────────────────────────────────────
// IEC61850 以逻辑节点/数据属性路径寻址，格式: LD/LN$FC$DO$DA
// 例如: MEAS/MMXU1$MX$PhV$phsA$cVal$mag$f
struct IEC61850Point {
    std::string name;
    std::string object_ref;   // 完整对象引用: LDName/LNName.DOName.DAName
    std::string fc;           // 功能约束: MX(测量) ST(状态) CO(控制) CF(配置)等
    float       scale         = 1.0f;
    float       offset        = 0.0f;
    std::string unit;
    std::string description;
};

// ── OPC-UA 配置 ─────────────────────────────────────────────────────────────────
// OPC-UA 节点以 NodeId 寻址，支持数字/字符串/GUID 三种形式
// 例如: ns=2;i=1001  或  ns=2;s=Temperature
struct OpcUaPoint {
    std::string name;
    std::string node_id;      // 节点ID字符串，如 "ns=2;i=1001" 或 "ns=2;s=Temp"
    float       scale         = 1.0f;
    float       offset        = 0.0f;
    std::string unit;
    std::string description;
};

// ── DL/T 645-2007 ───────────────────────────────────────────────────────────────
struct Dlt645Point {
    std::string name;
    std::string data_id;   // 8位十六进制数据标识，MSB在前，如 "02010100"
    float       scale   = 1.0f;
    float       offset  = 0.0f;
    std::string unit;
    std::string description;
};

// 总线上单块表的配置（地址 + 点表）
struct MeterConfig645 {
    std::string id;
    std::string meter_address  = "000000000001";  // 12位十进制BCD
    std::vector<Dlt645Point> points;
};

// ── DL/T 698.45 ─────────────────────────────────────────────────────────────────
struct Dlt698Point {
    std::string name;
    std::string oad;       // 8位十六进制OAD，如 "02010100"
    float       scale   = 1.0f;
    float       offset  = 0.0f;
    std::string unit;
    std::string description;
};

struct MeterConfig698 {
    std::string id;
    std::string meter_address  = "000000000001";
    std::vector<Dlt698Point> points;
};

// ── CAN / CAN FD ────────────────────────────────────────────────────────────────
// 信号按 DBC 语义定义。帧内位编号 = 字节序号*8 + 字节内位序号（0 = 该字节 LSB）。
//   LITTLE (Intel)   : start_bit 是信号最低位，取位时位号递增
//   BIG    (Motorola): start_bit 是信号最高位，字节内位号递减，
//                      递减到 bit0 后跳到下一字节的 bit7
enum class CanByteOrder { LITTLE, BIG };

struct CanSignal {
    std::string  name;
    uint32_t     can_id     = 0;      // 11 位标准帧 / 29 位扩展帧 ID（不含 flag 位）
    bool         extended   = false;  // true = 29 位扩展帧
    int          start_bit  = 0;
    int          bit_length = 16;     // 1..64
    CanByteOrder byte_order = CanByteOrder::LITTLE;
    bool         is_signed  = false;  // 按二进制补码解释并符号扩展
    double       scale      = 1.0;
    double       offset     = 0.0;
    std::string  unit;
    std::string  description;
};

// ── 公共时间戳工具 ──────────────────────────────────────────────────────────────
// 必须用 gmtime_r：gmtime 返回共享的静态 tm，而本函数被每个设备线程并发调用，
// 时间戳又会进入 MQTT 报文和缓存的 ts 列（时间段补传按 ts 检索）。
inline std::string nowIso() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::tm tmv{};
    gmtime_r(&t, &tmv);
    std::ostringstream oss;
    oss << std::put_time(&tmv, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

} // namespace industrial
