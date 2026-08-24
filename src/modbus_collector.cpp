// src/modbus_collector.cpp
#include "modbus_collector.h"
#include <modbus/modbus.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <vector>

namespace industrial {

ModbusCollector::ModbusCollector(const ModbusConfig& cfg) : cfg_(cfg) {}

ModbusCollector::~ModbusCollector() { disconnect(); }

void ModbusCollector::connect() {
    ctx_ = modbus_new_tcp(cfg_.host.c_str(), cfg_.port);
    if (!ctx_) throw std::runtime_error("modbus_new_tcp 分配失败");

    modbus_set_response_timeout(ctx_, cfg_.timeout_sec, 0);
    modbus_set_slave(ctx_, cfg_.unit_id);
    // 关闭 libmodbus 内部调试输出
    modbus_set_debug(ctx_, FALSE);

    if (modbus_connect(ctx_) == -1) {
        std::string err = modbus_strerror(errno);
        modbus_free(ctx_); ctx_ = nullptr;
        throw std::runtime_error("Modbus 连接失败 " + cfg_.host + ":"
                                 + std::to_string(cfg_.port) + " — " + err);
    }
    spdlog::info("Modbus TCP 已连接 {}:{} unit={}", cfg_.host, cfg_.port, cfg_.unit_id);
}

void ModbusCollector::disconnect() {
    if (ctx_) {
        modbus_close(ctx_);
        modbus_free(ctx_);
        ctx_ = nullptr;
        spdlog::info("Modbus 已断开");
    }
}

bool ModbusCollector::tryReconnect() {
    spdlog::warn("Modbus 尝试重连...");
    modbus_close(ctx_);
    if (modbus_connect(ctx_) == 0) {
        spdlog::info("Modbus 重连成功");
        return true;
    }
    spdlog::error("Modbus 重连失败: {}", modbus_strerror(errno));
    return false;
}

// ── 合并读分组构建（一次性，结果缓存在 groups_）────────────────────────────────
void ModbusCollector::buildGroups() {
    groups_.clear();

    // 收集所有点的 (fc, address, reg_count, pt_idx)
    struct Item { int fc, addr, count; size_t idx; };
    std::vector<Item> items;
    items.reserve(cfg_.points.size());
    for (size_t i = 0; i < cfg_.points.size(); ++i) {
        const auto& pt = cfg_.points[i];
        int cnt = (pt.data_type == ModbusDataType::INT32  ||
                   pt.data_type == ModbusDataType::UINT32 ||
                   pt.data_type == ModbusDataType::FLOAT32) ? 2 : 1;
        items.push_back({pt.func_code, pt.address, cnt, i});
    }

    // 按 (fc, address) 排序，保证同一 fc 的点连续出现
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b){
        return a.fc != b.fc ? a.fc < b.fc : a.addr < b.addr;
    });

    for (const auto& it : items) {
        int mx = (it.fc == 1 || it.fc == 2) ? MAX_BIT_READ : MAX_REG_READ;

        // 判断是否需要新建分组
        bool newGroup = groups_.empty()
            || groups_.back().fc != it.fc
            || it.addr > groups_.back().start + groups_.back().width + COALESCE_GAP
            || it.addr + it.count > groups_.back().start + mx;

        if (newGroup) {
            groups_.push_back({it.fc, it.addr, it.count, {}});
        }

        auto& g = groups_.back();
        int offset = it.addr - g.start;
        g.slots.push_back({it.idx, offset, it.count});
        int needed = offset + it.count;
        if (needed > g.width) g.width = needed;
    }

    groups_built_ = true;
    spdlog::info("Modbus [{}:{}] 合并读: {} 点 → {} 组请求（GAP≤{}）",
                 cfg_.host, cfg_.port,
                 cfg_.points.size(), groups_.size(), COALESCE_GAP);
}

// ── 按分组批量读取，返回与 cfg_.points 对齐的结果向量 ────────────────────────
DataPoints ModbusCollector::readGroups() {
    // 预分配并初始化（默认 BAD）
    DataPoints result(cfg_.points.size());
    for (size_t i = 0; i < cfg_.points.size(); ++i) {
        const auto& pt = cfg_.points[i];
        result[i].name        = pt.name;
        result[i].unit        = pt.unit;
        result[i].description = pt.description;
        result[i].timestamp   = nowIso();
        result[i].quality     = Quality::BAD;
        result[i].from_range  = pt.from_range;
    }

    for (const auto& g : groups_) {
        if (g.fc == 1 || g.fc == 2) {
            // ── 线圈 / 离散输入 ──────────────────────────────────────────────
            std::vector<uint8_t> bits(g.width, 0);
            int rc = (g.fc == 1)
                ? modbus_read_bits(ctx_, g.start, g.width, bits.data())
                : modbus_read_input_bits(ctx_, g.start, g.width, bits.data());
            if (rc != g.width) {
                spdlog::warn("Modbus FC{} 批量读失败 addr={} count={}: {}",
                             g.fc, g.start, g.width, modbus_strerror(errno));
                continue;
            }
            for (const auto& s : g.slots) {
                result[s.pt_idx].raw_value = static_cast<bool>(bits[s.offset]);
                result[s.pt_idx].value     = result[s.pt_idx].raw_value;
                result[s.pt_idx].quality   = Quality::GOOD;
            }
        } else {
            // ── 保持寄存器 / 输入寄存器 ──────────────────────────────────────
            std::vector<uint16_t> regs(g.width, 0);
            int rc = (g.fc == 4)
                ? modbus_read_input_registers(ctx_, g.start, g.width, regs.data())
                : modbus_read_registers(ctx_, g.start, g.width, regs.data());
            if (rc != g.width) {
                spdlog::warn("Modbus FC{} 批量读失败 addr={} count={}: {}",
                             g.fc, g.start, g.width, modbus_strerror(errno));
                continue;
            }
            for (const auto& s : g.slots) {
                const auto& pt  = cfg_.points[s.pt_idx];
                PointValue  raw = decodeRegisters(regs.data() + s.offset,
                                                  s.count, pt.data_type);
                double rawD = std::visit([](auto&& v) -> double {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>) return 0.0;
                    else return static_cast<double>(v);
                }, raw);
                result[s.pt_idx].raw_value = raw;
                result[s.pt_idx].value     = static_cast<float>(
                                                 applyScale(rawD, pt.scale, pt.offset));
                result[s.pt_idx].quality   = Quality::GOOD;
            }
        }
    }
    return result;
}

DataPoints ModbusCollector::readAll() {
    if (!groups_built_) buildGroups();

    DataPoints result = readGroups();

    int good = 0;
    for (auto& p : result) if (p.quality == Quality::GOOD) ++good;
    spdlog::info("Modbus 采集: {}/{} 点 GOOD", good, result.size());
    return result;
}

DataPoint ModbusCollector::readPoint(const ModbusPoint& pt) {
    DataPoint dp;
    dp.name        = pt.name;
    dp.unit        = pt.unit;
    dp.description = pt.description;
    dp.timestamp   = nowIso();
    dp.quality     = Quality::BAD;

    if (!ctx_) return dp;

    int count = (pt.data_type == ModbusDataType::INT32  ||
                 pt.data_type == ModbusDataType::UINT32 ||
                 pt.data_type == ModbusDataType::FLOAT32) ? 2 : 1;
    int rc = -1;

    if (pt.func_code == 1 || pt.func_code == 2) {
        uint8_t bits[8] = {};
        rc = (pt.func_code == 1)
             ? modbus_read_bits(ctx_, pt.address, 1, bits)
             : modbus_read_input_bits(ctx_, pt.address, 1, bits);
        if (rc == 1) {
            dp.raw_value = static_cast<bool>(bits[0]);
            dp.value     = dp.raw_value;
            dp.quality   = Quality::GOOD;
        }
    } else {
        uint16_t regs[2] = {};
        rc = (pt.func_code == 4)
             ? modbus_read_input_registers(ctx_, pt.address, count, regs)
             : modbus_read_registers(ctx_, pt.address, count, regs);
        if (rc == count) {
            PointValue raw = decodeRegisters(regs, count, pt.data_type);
            dp.raw_value   = raw;
            double rawD    = std::visit([](auto&& v) -> double {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) return 0.0;
                else return static_cast<double>(v);
            }, raw);
            dp.value   = static_cast<float>(applyScale(rawD, pt.scale, pt.offset));
            dp.quality = Quality::GOOD;
        }
    }

    if (dp.quality == Quality::BAD) {
        // 重连由外层 runDeviceForever 统一按用户配置的间隔处理，此处不自行重连
        spdlog::warn("读取 {} 失败: {}", pt.name, modbus_strerror(errno));
    } else {
        spdlog::debug("  {} = {} {}", pt.name,
                      dp.toDouble().value_or(0.0), pt.unit);
    }
    return dp;
}

PointValue ModbusCollector::decodeRegisters(const uint16_t* regs, int /*count*/,
                                              ModbusDataType dt) {
    switch (dt) {
        case ModbusDataType::BOOL:    return static_cast<bool>(regs[0] != 0);
        case ModbusDataType::UINT16:  return static_cast<uint32_t>(regs[0]);
        case ModbusDataType::INT16: {
            int16_t v; std::memcpy(&v, &regs[0], 2);
            return static_cast<int32_t>(v);
        }
        case ModbusDataType::UINT32:
            return static_cast<uint32_t>((regs[0] << 16) | regs[1]);
        case ModbusDataType::INT32: {
            uint32_t raw = (static_cast<uint32_t>(regs[0]) << 16) | regs[1];
            int32_t v; std::memcpy(&v, &raw, 4);
            return v;
        }
        case ModbusDataType::FLOAT32: {
            uint32_t raw = (static_cast<uint32_t>(regs[0]) << 16) | regs[1];
            float v; std::memcpy(&v, &raw, 4);
            return v;
        }
    }
    return {};
}

double ModbusCollector::applyScale(double raw, float scale, float offset) {
    return raw * scale + offset;
}

} // namespace industrial
