// src/dlt698_collector.cpp — DL/T 698.45 单表采集器（注入 transport，测试版）
#include "dlt698_collector.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstring>

namespace industrial {

static constexpr uint8_t FRAME_START   = 0x68;
static constexpr uint8_t FRAME_END     = 0x16;
static constexpr uint8_t CTRL_REQ      = 0x00;
static constexpr uint8_t SVC_CONNECT   = 0x40;
static constexpr uint8_t SVC_CONN_RESP = 0xC0;
static constexpr uint8_t SVC_GET_REQ   = 0x01;
static constexpr uint8_t SVC_GET_RESP  = 0x81;

uint16_t Dlt698Collector::crc16(const uint8_t* d, int len) {
    uint16_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= d[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc>>1)^0xA001 : crc>>1;
    }
    return crc;
}

Dlt698Collector::Dlt698Collector(std::shared_ptr<ITransport> transport,
                                  const MeterConfig698& meter,
                                  int timeout_ms)
    : transport_(std::move(transport)), meter_(meter), timeout_ms_(timeout_ms)
{
    std::string p = meter.meter_address;
    while (p.size() < 12) p = "0" + p;
    for (int i = 0; i < 6; i++)
        addr_[i] = (uint8_t)(((p[2*i]-'0')<<4)|(p[2*i+1]-'0'));
}

std::vector<uint8_t> Dlt698Collector::buildFrame(const std::vector<uint8_t>& inner) {
    uint16_t len = (uint16_t)inner.size();
    std::vector<uint8_t> ci = {(uint8_t)(len&0xFF),(uint8_t)(len>>8)};
    ci.insert(ci.end(), inner.begin(), inner.end());
    uint16_t crc = crc16(ci.data(),(int)ci.size());
    std::vector<uint8_t> f = {FRAME_START};
    f.insert(f.end(), ci.begin(), ci.end());
    f.push_back(crc&0xFF); f.push_back(crc>>8);
    f.push_back(FRAME_END);
    return f;
}

static std::vector<uint8_t> buildInner(uint8_t ser, const uint8_t addr[6],
                                        const std::vector<uint8_t>& appdu) {
    std::vector<uint8_t> v = {CTRL_REQ, ser, 0x06};
    for (int i=0;i<6;i++) v.push_back(addr[i]);
    v.push_back(0x06);
    for (int i=0;i<6;i++) v.push_back(0x00);
    v.insert(v.end(), appdu.begin(), appdu.end());
    return v;
}

bool Dlt698Collector::sendConnect() {
    std::vector<uint8_t> appdu = {SVC_CONNECT, 0x01, 0x00, 0x00};
    auto frame = buildFrame(buildInner(ser_++, addr_, appdu));
    transport_->flush();
    if (transport_->write(frame.data(),(int)frame.size()) != (int)frame.size())
        return false;
    auto resp = transact({});
    if (!resp.empty() && resp[0] == SVC_CONN_RESP) {
        spdlog::info("DLT698[{}] 应用连接成功", meter_.id);
        return true;
    }
    spdlog::debug("DLT698[{}] 连接无响应，继续 GET", meter_.id);
    return false;
}

std::vector<uint8_t> Dlt698Collector::buildGetRequest(const uint8_t oad[4]) {
    std::vector<uint8_t> appdu = {SVC_GET_REQ, ser_};
    appdu.insert(appdu.end(), oad, oad+4);
    return buildFrame(buildInner(ser_++, addr_, appdu));
}

std::vector<uint8_t> Dlt698Collector::transact(const std::vector<uint8_t>& frame) {
    if (!frame.empty()) {
        transport_->flush();
        if (transport_->write(frame.data(),(int)frame.size()) != (int)frame.size())
            return {};
    }
    // 整个事务共享一个截止时间。若每次 read 都传完整 timeout_ms，
    // 单次事务最坏要等 (64 跳过 + 1 长度 + 1 帧体) × timeout_ms。
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms_);
    auto remain  = [&] { return remainingMs(deadline); };
    // remainingMs 向上取整，故等待必然覆盖到 deadline，"预算耗尽"是确定性判据
    auto expired = [&] { return remain() == 0; };
    auto fail = [&](const char* stage) {
        if (expired())
            spdlog::warn("DLT698[{}] 事务超时（{}ms）：{}阶段未收全，丢弃本帧",
                         meter_.id, timeout_ms_, stage);
        else
            spdlog::warn("DLT698[{}] 读取中断：{}阶段", meter_.id, stage);
        return std::vector<uint8_t>{};
    };

    uint8_t b = 0;
    for (int skip=0; skip<64; ++skip) {
        if (transport_->read(&b,1,remain())!=1) return fail("帧起始符");
        if (b==FRAME_START) break;
    }
    if (b!=FRAME_START) {
        spdlog::warn("DLT698[{}] 连续 64 字节未见帧起始符 0x68（线路噪声？）", meter_.id);
        return {};
    }

    uint8_t lb[2];
    if (transport_->read(lb,2,remain())!=2) return fail("长度域");
    uint16_t inner_len = (uint16_t)(lb[0]|(lb[1]<<8));
    if (inner_len<3||inner_len>512) return {};

    std::vector<uint8_t> body(inner_len+3);
    if (transport_->read(body.data(),(int)body.size(),remain())!=(int)body.size())
        return fail("帧体");
    if (body[inner_len+2]!=FRAME_END) return {};

    std::vector<uint8_t> ci={lb[0],lb[1]};
    ci.insert(ci.end(), body.begin(), body.begin()+inner_len);
    if (crc16(ci.data(),(int)ci.size()) !=
        (uint16_t)(body[inner_len]|(body[inner_len+1]<<8))) {
        spdlog::warn("DLT698[{}] CRC 错误", meter_.id);
        return {};
    }

    const uint8_t* p = body.data();
    int rem = inner_len;
    if (rem < 2) return {};
    p += 2; rem -= 2;                          // skip CTRL, SER
    if (rem < 1) return {};
    int dl = *p; p++; rem--;                   // DSA len
    if (rem < dl) return {};
    p += dl; rem -= dl;
    if (rem < 1) return {};
    int sl = *p; p++; rem--;                   // SSA len
    if (rem < sl) return {};
    p += sl; rem -= sl;
    return std::vector<uint8_t>(p, p+rem);
}

bool Dlt698Collector::parseGetResponse(const std::vector<uint8_t>& ap, double& out) {
    if (ap.size()<9||ap[0]!=SVC_GET_RESP) return false;
    if (ap[6]!=0x00) {
        spdlog::warn("DLT698[{}] GET 错误码 0x{:02x}", meter_.id, ap[6]);
        return false;
    }
    return decodeTypedData(ap.data()+7,(int)ap.size()-7, out);
}

bool Dlt698Collector::decodeTypedData(const uint8_t* p, int len, double& out) {
    if (len<1) return false;
    uint8_t tag=p[0]; const uint8_t* d=p+1; int dl=len-1;
    switch(tag){
        case 0x03: if(dl<1)return false; out=d[0]?1.0:0.0; return true;
        case 0x0F: if(dl<1)return false; out=(int8_t)d[0]; return true;
        case 0x10: if(dl<2)return false; out=(int16_t)(d[0]|(d[1]<<8)); return true;
        case 0x15: if(dl<4)return false;
            out=(int32_t)((uint32_t)d[0]|((uint32_t)d[1]<<8)|
                         ((uint32_t)d[2]<<16)|((uint32_t)d[3]<<24)); return true;
        case 0x17: if(dl<8)return false; {
            int64_t v=0; for(int i=7;i>=0;i--) v=(v<<8)|d[i];
            out=(double)v; } return true;
        case 0x11: if(dl<1)return false; out=d[0]; return true;
        case 0x12: if(dl<2)return false; out=(uint16_t)(d[0]|(d[1]<<8)); return true;
        case 0x16: if(dl<4)return false;
            out=(uint32_t)(d[0]|((uint32_t)d[1]<<8)|
                          ((uint32_t)d[2]<<16)|((uint32_t)d[3]<<24)); return true;
        case 0x18: if(dl<8)return false; {
            uint64_t v=0; for(int i=7;i>=0;i--) v=(v<<8)|d[i];
            out=(double)v; } return true;
        case 0x06: if(dl<4)return false; {
            uint32_t r=(uint32_t)d[0]|((uint32_t)d[1]<<8)|
                       ((uint32_t)d[2]<<16)|((uint32_t)d[3]<<24);
            float fv; memcpy(&fv,&r,4); out=fv; } return true;
        case 0x07: if(dl<8)return false; {
            uint64_t r=0; for(int i=7;i>=0;i--) r=(r<<8)|d[i];
            double dv; memcpy(&dv,&r,8); out=dv; } return true;
        case 0x09: if(dl<1)return false; {
            int sl=d[0]; if(dl<1+sl)return false;
            double res=0;
            for(int i=0;i<sl;i++) res=res*100+((d[1+i]>>4)*10+(d[1+i]&0xF));
            out=res; } return true;
        default:
            spdlog::debug("DLT698[{}] 未知类型 tag=0x{:02x}", meter_.id, tag);
            return false;
    }
}

DataPoint Dlt698Collector::readPoint(const Dlt698Point& pt) {
    DataPoint dp;
    dp.name=pt.name; dp.unit=pt.unit;
    dp.description=pt.description; dp.timestamp=nowIso();
    dp.quality=Quality::BAD;

    uint32_t ov=0;
    try { ov=(uint32_t)std::stoul(pt.oad,nullptr,16); } catch(...) { return dp; }
    uint8_t oad[4]={(uint8_t)(ov>>24),(uint8_t)(ov>>16),(uint8_t)(ov>>8),(uint8_t)ov};

    auto appdu = transact(buildGetRequest(oad));
    if (appdu.empty()) return dp;

    double raw=0;
    if (!parseGetResponse(appdu, raw)) return dp;

    dp.raw_value = static_cast<float>(raw);
    dp.value     = static_cast<float>(raw*pt.scale+pt.offset);
    dp.quality   = Quality::GOOD;
    return dp;
}

DataPoints Dlt698Collector::readAll() {
    DataPoints result;
    result.reserve(meter_.points.size());
    int good=0;
    for (const auto& pt : meter_.points) {
        auto dp=readPoint(pt);
        if (dp.quality==Quality::GOOD) ++good;
        result.push_back(std::move(dp));
    }
    spdlog::info("DLT698[{}] {}/{} 点 GOOD", meter_.id, good, result.size());
    return result;
}

} // namespace industrial
