#pragma once
/**
 * modbus_collector.h — Modbus TCP 采集器（libmodbus 静态链接）
 */
#include "types.h"
#include "config.h"

struct _modbus;

namespace industrial {

class ModbusCollector {
public:
    explicit ModbusCollector(const ModbusConfig& cfg);
    ~ModbusCollector();

    void       connect();
    void       disconnect();
    DataPoints readAll();

    double pollInterval() const { return cfg_.poll_interval; }

private:
    // 一次批量请求覆盖的地址窗口
    struct RegSlot {
        size_t pt_idx;   // cfg_.points 中的下标（决定结果写入位置）
        int    offset;   // 在读取缓冲区中的起始寄存器偏移
        int    count;    // 占用寄存器数（1 或 2）
    };
    struct ReadGroup {
        int  fc;         // Modbus func_code（1/2/3/4）
        int  start;      // 批量读起始地址
        int  width;      // 批量读寄存器/bit 总数
        std::vector<RegSlot> slots;
    };

    // 协议规定单次上限；gap 以内的空洞合并读（避免拆成多组）
    static constexpr int MAX_REG_READ  = 125;
    static constexpr int MAX_BIT_READ  = 256;
    static constexpr int COALESCE_GAP  = 10;

    ModbusConfig           cfg_;
    _modbus*               ctx_ = nullptr;
    std::vector<ReadGroup> groups_;
    bool                   groups_built_ = false;

    void       buildGroups();
    DataPoints readGroups();
    DataPoint  readPoint(const ModbusPoint& pt);   // 保留，错误降级用
    bool       tryReconnect();

    static PointValue decodeRegisters(const uint16_t* regs, int count,
                                       ModbusDataType dt);
    static double     applyScale(double raw, float scale, float offset);
};

} // namespace industrial
