// src/iec104_collector.cpp — IEC 60870-5-104 原生 socket 实现
#include "iec104_collector.h"
#include <spdlog/spdlog.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <limits>

namespace industrial {

// ── 小工具 ───────────────────────────────────────────────────────────────────
static uint32_t readLE32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
}

// ── 构造 / 析构 ───────────────────────────────────────────────────────────────
IEC104Collector::IEC104Collector(const IEC104Config& cfg) : cfg_(cfg) {}
IEC104Collector::~IEC104Collector() { disconnect(); }

// ── 连接 ─────────────────────────────────────────────────────────────────────
void IEC104Collector::connect() {
    sockfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) throw std::runtime_error("socket() 失败");

    struct timeval tv{ cfg_.timeout_sec, 0 };
    ::setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sockfd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(uint16_t(cfg_.port));

    if (::inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr) != 1) {
        hostent* he = ::gethostbyname(cfg_.host.c_str());
        if (!he) { ::close(sockfd_); sockfd_ = -1;
            throw std::runtime_error("无法解析主机: " + cfg_.host); }
        std::memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(in_addr));
    }

    if (::connect(sockfd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string e = strerror(errno);
        ::close(sockfd_); sockfd_ = -1;
        throw std::runtime_error("IEC104 连接失败 " + cfg_.host + ":"
                                 + std::to_string(cfg_.port) + " — " + e);
    }
    spdlog::info("IEC104 TCP 已连接 {}:{} CA={}", cfg_.host, cfg_.port, cfg_.common_address);

    running_ = true;
    recvThread_ = std::thread(&IEC104Collector::recvLoop, this);

    sendStartDT();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    sendGI();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
}

// ── 断开 ─────────────────────────────────────────────────────────────────────
void IEC104Collector::disconnect() {
    running_ = false;
    if (sockfd_ >= 0) {
        ::shutdown(sockfd_, SHUT_RDWR);
        ::close(sockfd_);
        sockfd_ = -1;
    }
    if (recvThread_.joinable()) recvThread_.join();
    spdlog::info("IEC104 已断开");
}

// ── 读所有点 ─────────────────────────────────────────────────────────────────
DataPoints IEC104Collector::readAll() {
    sendGI();
    std::this_thread::sleep_for(
        std::chrono::milliseconds(int(cfg_.poll_interval * 400)));

    DataPoints result;
    result.reserve(cfg_.points.size());

    std::lock_guard<std::mutex> lk(cacheMtx_);
    for (const auto& pt : cfg_.points) {
        DataPoint dp;
        dp.name        = pt.name;
        dp.unit        = pt.unit;
        dp.description = pt.description;
        dp.timestamp   = nowIso();

        auto it = cache_.find(pt.ioa);
        if (it != cache_.end()) {
            double eng = applyScale(it->second.value, pt.scale, pt.offset);
            dp.value     = static_cast<float>(eng);
            dp.raw_value = static_cast<float>(it->second.value);
            dp.quality   = flagsToQuality(it->second.flags);
        } else {
            dp.quality = Quality::BAD;
        }
        spdlog::debug("  IEC104 {} IOA={} = {}", pt.name, pt.ioa,
                      dp.toDouble().value_or(std::numeric_limits<double>::quiet_NaN()));
        result.push_back(std::move(dp));
    }
    int good = 0;
    for (auto& p : result) if (p.quality == Quality::GOOD) ++good;
    spdlog::info("IEC104 采集: {}/{} 点 GOOD", good, result.size());
    return result;
}

// ── 发送原语 ─────────────────────────────────────────────────────────────────
void IEC104Collector::sendStartDT() {
    uint8_t f[] = {0x68, 0x04, 0x07, 0x00, 0x00, 0x00};
    if (sendRaw(f, sizeof(f))) spdlog::debug("IEC104 → STARTDT_ACT");
}

void IEC104Collector::sendGI() {
    // C_IC_NA_1 TypeID=100, COT=6(激活), QOI=20
    uint8_t ca_lo = uint8_t(cfg_.common_address & 0xFF);
    uint8_t ca_hi = uint8_t((cfg_.common_address >> 8) & 0xFF);
    uint8_t c0 = uint8_t((sendSeq_ << 1) & 0xFF);
    uint8_t c1 = uint8_t((sendSeq_ >> 7) & 0xFF);
    uint8_t c2 = uint8_t((recvSeq_ << 1) & 0xFF);
    uint8_t c3 = uint8_t((recvSeq_ >> 7) & 0xFF);
    ++sendSeq_;
    uint8_t asdu[] = {0x64,0x01,0x06,0x00, ca_lo,ca_hi, 0x00,0x00,0x00, 0x14};
    uint8_t frame[6 + sizeof(asdu)];
    frame[0]=0x68; frame[1]=uint8_t(4+sizeof(asdu));
    frame[2]=c0; frame[3]=c1; frame[4]=c2; frame[5]=c3;
    std::memcpy(frame+6, asdu, sizeof(asdu));
    if (sendRaw(frame, sizeof(frame))) spdlog::debug("IEC104 → 总召唤");
}

void IEC104Collector::sendSFrame() {
    uint8_t c2 = uint8_t((recvSeq_ << 1) & 0xFF);
    uint8_t c3 = uint8_t((recvSeq_ >> 7) & 0xFF);
    uint8_t f[] = {0x68,0x04,0x01,0x00,c2,c3};
    sendRaw(f, sizeof(f));
}

void IEC104Collector::sendTestFrCon() {
    uint8_t f[] = {0x68,0x04,0x83,0x00,0x00,0x00};
    sendRaw(f, sizeof(f));
}

bool IEC104Collector::sendRaw(const uint8_t* data, size_t len) {
    if (sockfd_ < 0) return false;
    return ::send(sockfd_, data, len, MSG_NOSIGNAL) > 0;
}

bool IEC104Collector::recvExact(uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len && running_) {
        ssize_t n = ::recv(sockfd_, buf+got, len-got, 0);
        if (n <= 0) return false;
        got += size_t(n);
    }
    return got == len;
}

// ── 接收主循环 ────────────────────────────────────────────────────────────────
void IEC104Collector::recvLoop() {
    spdlog::debug("IEC104 接收线程启动");
    while (running_ && sockfd_ >= 0) {
        uint8_t hdr[2];
        if (!recvExact(hdr, 2)) {
            if (running_) spdlog::warn("IEC104 连接断开");
            break;
        }
        if (hdr[0] != 0x68) continue;
        uint8_t len = hdr[1];
        if (len < 4) continue;
        std::vector<uint8_t> body(len);
        if (!recvExact(body.data(), len)) break;
        parseAPDU(body.data(), len);
    }
    spdlog::debug("IEC104 接收线程退出");
}

// ── 解析 APDU ─────────────────────────────────────────────────────────────────
void IEC104Collector::parseAPDU(const uint8_t* body, size_t len) {
    if (len < 4) return;
    uint8_t c0 = body[0];

    if (c0 & 0x01) {
        if ((c0 & 0x03) == 0x03) {
            // U帧
            if (c0 == 0x0B) spdlog::debug("IEC104 ← STARTDT_CON");
            else if (c0 == 0x43) sendTestFrCon(); // TESTFR_ACT → CON
        }
        // S帧：忽略
        return;
    }

    // I帧
    if (len < 6) return;
    uint16_t ns = uint16_t((body[0] >> 1) | (body[1] << 7));
    recvSeq_ = uint16_t(ns + 1);
    sendSFrame();

    if (len > 4) parseASDU(body + 4, len - 4);
}

// ── 解析 ASDU ─────────────────────────────────────────────────────────────────
void IEC104Collector::parseASDU(const uint8_t* asdu, size_t len) {
    if (len < 6) return;
    uint8_t typeId   = asdu[0];
    uint8_t vsq      = asdu[1];
    int     numElem  = vsq & 0x7F;
    // bool sq       = (vsq >> 7) & 1;

    size_t off = 6; // 跳过 TypeID VSQ COT ORG CA(2B)
    for (int i = 0; i < numElem && off + 3 <= len; ++i) {
        int ioa = int(asdu[off]) | (int(asdu[off+1])<<8) | (int(asdu[off+2])<<16);
        off += 3;
        if (off >= len) break;
        auto e = decodeElement(typeId, asdu + off, len - off);
        if (e.valid) {
            std::lock_guard<std::mutex> lk(cacheMtx_);
            cache_[ioa] = {e.value, e.flags};
            spdlog::debug("  IEC104 ← IOA={} type={} val={:.4f}", ioa, typeId, e.value);
        }
        off += size_t(e.consumed);
    }
}

// ── 元素解码 ─────────────────────────────────────────────────────────────────
IEC104Collector::ElemResult
IEC104Collector::decodeElement(uint8_t tid, const uint8_t* d, size_t avail) {
    ElemResult r;
    switch (tid) {
        case 1:  // M_SP_NA_1  单点遥信
            if (avail < 1) break;
            r.value = (d[0] & 0x01) ? 1.0 : 0.0;
            r.flags = (d[0] >> 4) & 0x0F;
            r.consumed = 1; r.valid = true; break;

        case 3:  // M_DP_NA_1  双点遥信
            if (avail < 1) break;
            r.value = double(d[0] & 0x03);
            r.flags = (d[0] >> 4) & 0x0F;
            r.consumed = 1; r.valid = true; break;

        case 11: // M_ME_NB_1  规一化值 2B+QDS
            if (avail < 3) break;
            { int16_t v; std::memcpy(&v, d, 2);
              r.value = double(v) / 32767.0; }
            r.flags = d[2];
            r.consumed = 3; r.valid = true; break;

        case 13: // M_ME_NC_1  短浮点 4B+QDS
            if (avail < 5) break;
            { float v; std::memcpy(&v, d, 4); r.value = double(v); }
            r.flags = d[4];
            r.consumed = 5; r.valid = true; break;

        case 15: // M_IT_NA_1  累计量 4B+SCQ
            if (avail < 5) break;
            r.value    = double(readLE32(d) & 0x00FFFFFFu);
            r.flags    = d[4];
            r.consumed = 5; r.valid = true; break;

        default:
            r.consumed = 1; break;
    }
    return r;
}

double IEC104Collector::applyScale(double raw, float scale, float offset) {
    return raw * scale + offset;
}

Quality IEC104Collector::flagsToQuality(uint8_t flags) {
    if (flags & 0x80) return Quality::BAD;
    if (flags & 0x10) return Quality::UNCERTAIN;
    return Quality::GOOD;
}

} // namespace industrial
