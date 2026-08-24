// src/iec61850_collector.cpp — IEC 61850 MMS 客户端实现
// 基于 libiec61850 C API

#include "iec61850_collector.h"
#include <spdlog/spdlog.h>

// libiec61850 C 头文件
#include <iec61850_client.h>
#include <mms_value.h>

#include <stdexcept>
#include <cstring>
#include <chrono>
#include <thread>

namespace industrial {

// ── FunctionalConstraint 字符串转枚举 ─────────────────────────────────────────
static FunctionalConstraint fcFromStr(const std::string& s) {
    if (s == "ST") return IEC61850_FC_ST;
    if (s == "MX") return IEC61850_FC_MX;
    if (s == "SP") return IEC61850_FC_SP;
    if (s == "SV") return IEC61850_FC_SV;
    if (s == "CF") return IEC61850_FC_CF;
    if (s == "DC") return IEC61850_FC_DC;
    if (s == "SG") return IEC61850_FC_SG;
    if (s == "SE") return IEC61850_FC_SE;
    if (s == "SR") return IEC61850_FC_SR;
    if (s == "OR") return IEC61850_FC_OR;
    if (s == "BL") return IEC61850_FC_BL;
    if (s == "EX") return IEC61850_FC_EX;
    if (s == "CO") return IEC61850_FC_CO;
    return IEC61850_FC_MX;  // 默认测量值
}

// ── MMS 值解码 → double ────────────────────────────────────────────────────────
double IEC61850Collector::decodeMmsValue(MmsValue* val) {
    if (!val) return 0.0;
    switch (MmsValue_getType(val)) {
        case MMS_BOOLEAN:
            return MmsValue_getBoolean(val) ? 1.0 : 0.0;
        case MMS_INTEGER:
            return static_cast<double>(MmsValue_toInt64(val));
        case MMS_UNSIGNED:
            return static_cast<double>(MmsValue_toUint32(val));
        case MMS_FLOAT:
            return static_cast<double>(MmsValue_toFloat(val));
        case MMS_UTC_TIME: {
            // 返回 Unix 时间戳（秒）
            uint64_t ms = MmsValue_getUtcTimeInMs(val);
            return static_cast<double>(ms) / 1000.0;
        }
        case MMS_BIT_STRING:
            return static_cast<double>(MmsValue_getBitStringAsInteger(val));
        case MMS_VISIBLE_STRING:
        case MMS_STRING: {
            const char* s = MmsValue_toString(val);
            if (s) try { return std::stod(s); } catch (...) {}
            return 0.0;
        }
        case MMS_STRUCTURE: {
            // 结构体：尝试读取第一个子成员（常见于 AnalogValue/mag/f）
            MmsValue* child = MmsValue_getElement(val, 0);
            if (child) return decodeMmsValue(child);
            return 0.0;
        }
        default:
            return 0.0;
    }
}

// ── 质量解码 ─────────────────────────────────────────────────────────────────
Quality IEC61850Collector::decodeQuality(MmsValue* qualVal) {
    if (!qualVal) return Quality::BAD;
    // quality 是一个 BIT STRING，bit0=validity
    // validity: 00=good 01=invalid 10=reserved 11=questionable
    if (MmsValue_getType(qualVal) == MMS_BIT_STRING) {
        uint32_t bits = MmsValue_getBitStringAsInteger(qualVal);
        uint32_t validity = (bits >> 6) & 0x03;  // bits 6-7（MSB first）
        if (validity == 0) return Quality::GOOD;
        if (validity == 3) return Quality::UNCERTAIN;
        return Quality::BAD;
    }
    return Quality::GOOD;
}

double IEC61850Collector::applyScale(double raw, float scale, float offset) {
    return raw * scale + offset;
}

// ── 构造 / 析构 ───────────────────────────────────────────────────────────────
IEC61850Collector::IEC61850Collector(const IEC61850Config& cfg) : cfg_(cfg) {}

IEC61850Collector::~IEC61850Collector() { disconnect(); }

// ── 连接 ─────────────────────────────────────────────────────────────────────
void IEC61850Collector::connect() {
    IedClientError err;
    conn_ = IedConnection_create();
    if (!conn_) throw std::runtime_error("IedConnection_create 失败");

    // 设置连接超时
    IedConnection_setConnectTimeout(conn_,
        static_cast<uint32_t>(cfg_.timeout_ms));

    IedConnection_connect(conn_, &err,
        cfg_.host.c_str(), cfg_.port);

    if (err != IED_ERROR_OK) {
        IedConnection_destroy(conn_);
        conn_ = nullptr;
        throw std::runtime_error(
            "IEC61850 连接失败 " + cfg_.host + ":" +
            std::to_string(cfg_.port) +
            " error=" + std::to_string(err));
    }

    connected_ = true;
    spdlog::info("IEC61850 MMS 已连接 {}:{}", cfg_.host, cfg_.port);

    // 如果开启报告模式，启用 BRCB 订阅
    if (cfg_.use_reports) {
        try { enableReports(); }
        catch (const std::exception& e) {
            spdlog::warn("IEC61850 报告订阅失败，降级为轮询: {}", e.what());
        }
    }
}

// ── 断开 ─────────────────────────────────────────────────────────────────────
void IEC61850Collector::disconnect() {
    if (conn_) {
        if (cfg_.use_reports) {
            try { disableReports(); } catch (...) {}
        }
        IedConnection_close(conn_);
        IedConnection_destroy(conn_);
        conn_      = nullptr;
        connected_ = false;
        spdlog::info("IEC61850 已断开");
    }
}

// ── 读所有点 ─────────────────────────────────────────────────────────────────
DataPoints IEC61850Collector::readAll() {
    DataPoints result;
    result.reserve(cfg_.points.size());

    for (const auto& pt : cfg_.points) {
        result.push_back(readPoint(pt));
    }

    int good = 0;
    for (auto& p : result) if (p.quality == Quality::GOOD) ++good;
    spdlog::info("IEC61850 采集: {}/{} 点 GOOD", good, result.size());
    return result;
}

// ── 读单个数据属性 ────────────────────────────────────────────────────────────
DataPoint IEC61850Collector::readPoint(const IEC61850Point& pt) {
    DataPoint dp;
    dp.name        = pt.name;
    dp.unit        = pt.unit;
    dp.description = pt.description;
    dp.timestamp   = nowIso();
    dp.quality     = Quality::BAD;

    if (!connected_ || !conn_) return dp;

    // 报告模式：直接从缓存读取
    if (cfg_.use_reports) {
        std::lock_guard<std::mutex> lk(cacheMtx_);
        auto it = cache_.find(pt.object_ref);
        if (it != cache_.end()) {
            double eng = applyScale(it->second.value, pt.scale, pt.offset);
            dp.value     = static_cast<float>(eng);
            dp.raw_value = static_cast<float>(it->second.value);
            dp.quality   = it->second.quality;
            dp.timestamp = it->second.timestamp;
        }
        return dp;
    }

    // 轮询模式：MMS ReadRequest
    IedClientError err = IED_ERROR_OK;
    FunctionalConstraint fc = fcFromStr(pt.fc);

    MmsValue* val = IedConnection_readObject(
        conn_, &err,
        pt.object_ref.c_str(), fc);

    if (err != IED_ERROR_OK || !val) {
        spdlog::warn("IEC61850 读取 {} 失败 err={}", pt.object_ref, (int)err);
        // 重连交由外层 runDeviceForever 统一处理
        return dp;
    }

    // 请求成功但对象不存在/无权限时，返回的是 MMS_DATA_ACCESS_ERROR，
    // 必须判 BAD，否则会把失败读当成 0.0 GOOD（曾导致正弦值恒为 0）。
    if (MmsValue_getType(val) == MMS_DATA_ACCESS_ERROR) {
        spdlog::warn("IEC61850 {} 数据访问错误（检查 object_ref/fc）", pt.object_ref);
        MmsValue_delete(val);
        return dp;
    }

    double raw = decodeMmsValue(val);
    MmsValue_delete(val);

    dp.raw_value = static_cast<float>(raw);
    dp.value     = static_cast<float>(applyScale(raw, pt.scale, pt.offset));
    dp.quality   = Quality::GOOD;

    spdlog::debug("  IEC61850 {} = {} {}", pt.name,
                  dp.toDouble().value_or(0.0), pt.unit);
    return dp;
}

// ── 报告回调（静态）──────────────────────────────────────────────────────────
void IEC61850Collector::reportCallback(void* userData,
                                        int /*reason*/,
                                        const char* objRef,
                                        MmsValue* val)
{
    if (!userData || !objRef || !val) return;
    auto* self = static_cast<IEC61850Collector*>(userData);

    double raw = decodeMmsValue(val);
    MmsCache mc;
    mc.value     = raw;
    mc.quality   = Quality::GOOD;
    mc.timestamp = nowIso();

    std::lock_guard<std::mutex> lk(self->cacheMtx_);
    self->cache_[objRef] = mc;
}

// ── 启用 BRCB 缓冲报告 ────────────────────────────────────────────────────────
void IEC61850Collector::enableReports() {
    // RCB 订阅在此版本中未实现，use_reports=true 时请升级此函数
    spdlog::warn("IEC61850 报告订阅（use_reports）暂未实现，使用轮询模式");
}

void IEC61850Collector::disableReports() {
}

} // namespace industrial
