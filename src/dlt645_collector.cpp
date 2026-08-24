// src/dlt645_collector.cpp — DL/T 645-2007 单表采集器（注入 transport）
#include "dlt645_collector.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <stdexcept>
#include <cstring>

namespace industrial {

static constexpr uint8_t FRAME_START = 0x68;
static constexpr uint8_t FRAME_END   = 0x16;
static constexpr uint8_t CTRL_READ   = 0x11;
static constexpr uint8_t CTRL_ERR    = 0x40;

Dlt645Collector::Dlt645Collector(std::shared_ptr<ITransport> transport,
                                  const MeterConfig645& meter,
                                  int timeout_ms)
    : transport_(std::move(transport)), meter_(meter), timeout_ms_(timeout_ms)
{
    parseMeterAddr(meter_.meter_address, addr_);
}

bool Dlt645Collector::parseMeterAddr(const std::string& s, uint8_t out[6]) {
    std::string p = s;
    while (p.size() < 12) p = "0" + p;
    if (p.size() != 12) { memset(out, 0, 6); return false; }
    uint8_t natural[6];
    for (int i = 0; i < 6; i++) {
        int hi = p[2*i]   - '0';
        int lo = p[2*i+1] - '0';
        if (hi < 0||hi > 9||lo < 0||lo > 9) { memset(out,0,6); return false; }
        natural[i] = (uint8_t)((hi<<4)|lo);
    }
    for (int i = 0; i < 6; i++) out[i] = natural[5-i];
    return true;
}

std::vector<uint8_t> Dlt645Collector::buildReadFrame(uint32_t di_val) {
    std::vector<uint8_t> f;
    f.reserve(16);
    for (int i = 0; i < 4; i++) f.push_back(0xFE);  // 唤醒
    f.push_back(FRAME_START);
    for (int i = 0; i < 6; i++) f.push_back(addr_[i]);
    f.push_back(FRAME_START);
    f.push_back(CTRL_READ);
    f.push_back(0x04);
    f.push_back(((di_val >>  0)&0xFF)+0x33);
    f.push_back(((di_val >>  8)&0xFF)+0x33);
    f.push_back(((di_val >> 16)&0xFF)+0x33);
    f.push_back(((di_val >> 24)&0xFF)+0x33);
    uint8_t cs = 0;
    for (size_t i = 4; i < f.size(); i++) cs += f[i];
    f.push_back(cs);
    f.push_back(FRAME_END);
    return f;
}

std::vector<uint8_t> Dlt645Collector::transact(const std::vector<uint8_t>& req) {
    transport_->flush();
    if (transport_->write(req.data(),(int)req.size()) != (int)req.size()) {
        spdlog::warn("DLT645[{}] 写入失败", meter_.id);
        return {};
    }

    // 整个事务共享一个截止时间。若每次 read 都传完整 timeout_ms，
    // 单次事务最坏要等 (32 跳过 + 1 帧头 + 1 帧体) × timeout_ms。
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms_);
    auto remain  = [&] { return remainingMs(deadline); };
    // remainingMs 向上取整，故等待必然覆盖到 deadline，"预算耗尽"是确定性判据
    auto expired = [&] { return remain() == 0; };
    auto fail = [&](const char* stage) {
        if (expired())
            spdlog::warn("DLT645[{}] 事务超时（{}ms）：{}阶段未收全，丢弃本帧",
                         meter_.id, timeout_ms_, stage);
        else
            spdlog::warn("DLT645[{}] 读取中断：{}阶段", meter_.id, stage);
        return std::vector<uint8_t>{};
    };

    // 跳过非 0x68 前导字节
    uint8_t b = 0; int skip = 0;
    while (skip++ < 32) {
        if (transport_->read(&b, 1, remain()) != 1) return fail("帧起始符");
        if (b == FRAME_START) break;
    }
    if (b != FRAME_START) {
        spdlog::warn("DLT645[{}] 连续 32 字节未见帧起始符 0x68（线路噪声？）", meter_.id);
        return {};
    }

    uint8_t hdr[9];
    if (transport_->read(hdr, 9, remain()) != 9) return fail("帧头");
    if (hdr[6] != FRAME_START) return {};

    uint8_t ctrl = hdr[7], len = hdr[8];
    if (ctrl & CTRL_ERR) {
        spdlog::warn("DLT645[{}] 错误响应 CTRL=0x{:02x}", meter_.id, ctrl);
        return {};
    }
    if (len < 4) return {};

    std::vector<uint8_t> data(len+2);
    if (transport_->read(data.data(), len+2, remain()) != len+2) return fail("帧体");
    if (data[len+1] != FRAME_END) return {};

    uint8_t cs = FRAME_START;
    for (int i = 0; i < 6; i++) cs += hdr[i];
    cs += FRAME_START; cs += ctrl; cs += len;
    for (int i = 0; i < len; i++) cs += data[i];
    if (cs != data[len]) {
        spdlog::warn("DLT645[{}] 校验和错误", meter_.id);
        return {};
    }

    std::vector<uint8_t> values;
    values.reserve(len-4);
    for (int i = 4; i < len; i++) values.push_back(data[i]-0x33);
    return values;
}

double Dlt645Collector::bcdToDouble(const uint8_t* bytes, int len) {
    double result = 0, mult = 1;
    for (int i = 0; i < len; i++) {
        result += ((bytes[i]>>4)*10 + (bytes[i]&0x0F)) * mult;
        mult *= 100;
    }
    return result;
}

DataPoint Dlt645Collector::readPoint(const Dlt645Point& pt) {
    DataPoint dp;
    dp.name = pt.name; dp.unit = pt.unit;
    dp.description = pt.description; dp.timestamp = nowIso();
    dp.quality = Quality::BAD;

    uint32_t di = 0;
    try { di = (uint32_t)std::stoul(pt.data_id, nullptr, 16); }
    catch (...) { return dp; }

    auto values = transact(buildReadFrame(di));
    if (values.empty()) return dp;

    double raw = bcdToDouble(values.data(),(int)values.size());
    dp.raw_value = static_cast<float>(raw);
    dp.value     = static_cast<float>(raw * pt.scale + pt.offset);
    dp.quality   = Quality::GOOD;
    return dp;
}

DataPoints Dlt645Collector::readAll() {
    DataPoints result;
    result.reserve(meter_.points.size());
    int good = 0;
    for (const auto& pt : meter_.points) {
        auto dp = readPoint(pt);
        if (dp.quality == Quality::GOOD) ++good;
        result.push_back(std::move(dp));
    }
    spdlog::info("DLT645[{}] {}/{} 点 GOOD", meter_.id, good, result.size());
    return result;
}

} // namespace industrial
