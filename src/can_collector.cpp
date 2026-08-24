// src/can_collector.cpp — CAN / CAN FD 采集器（Linux SocketCAN）
#include "can_collector.h"

#include <spdlog/spdlog.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <set>
#include <stdexcept>
#include <vector>

namespace industrial {

// 接收线程 poll 超时：决定 disconnect() 最坏等待时间
static constexpr int POLL_TIMEOUT_MS = 200;

// ── 信号位域提取（DBC 语义）───────────────────────────────────────────────────
std::optional<int64_t> canExtractRaw(const uint8_t* data, int data_len,
                                     const CanSignal& sig)
{
    const int len = sig.bit_length;
    if (len <= 0 || len > 64)   return std::nullopt;
    if (sig.start_bit < 0)      return std::nullopt;
    if (data_len <= 0)          return std::nullopt;

    const int nbits = data_len * 8;
    auto bitAt = [data](int b) -> uint64_t {
        return (data[b >> 3] >> (b & 7)) & 1u;
    };

    uint64_t raw = 0;
    if (sig.byte_order == CanByteOrder::LITTLE) {
        // Intel：start_bit 是 LSB，位号递增
        if (sig.start_bit + len > nbits) return std::nullopt;
        for (int k = 0; k < len; ++k)
            raw |= bitAt(sig.start_bit + k) << k;
    } else {
        // Motorola：start_bit 是 MSB，字节内位号递减，到 bit0 后跳下一字节 bit7
        int pos = sig.start_bit;
        for (int k = 0; k < len; ++k) {
            if (pos < 0 || pos >= nbits) return std::nullopt;
            raw = (raw << 1) | bitAt(pos);
            if ((pos & 7) == 0) pos += 15;   // 跨到下一字节的 bit7
            else                --pos;
        }
    }

    // 二进制补码符号扩展（len==64 时无需扩展，转型即为有符号值）
    if (sig.is_signed && len < 64 && ((raw >> (len - 1)) & 1))
        raw |= ~((uint64_t(1) << len) - 1);

    return static_cast<int64_t>(raw);
}

// ── 构造 / 析构 ───────────────────────────────────────────────────────────────
CanCollector::CanCollector(const CanConfig& cfg) : cfg_(cfg) {}

CanCollector::~CanCollector() { disconnect(); }

uint32_t CanCollector::slotKey(uint32_t can_id, bool extended) {
    // 扩展帧 ID 最大 0x1FFFFFFF，最高位空闲，借用为标准/扩展区分位
    return extended ? ((can_id & CAN_EFF_MASK) | CAN_EFF_FLAG)
                    :  (can_id & CAN_SFF_MASK);
}

// ── 连接 ──────────────────────────────────────────────────────────────────────
void CanCollector::connect() {
    disconnect();   // 幂等：重连时先清理上一轮 socket/线程

    int s = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0)
        throw std::runtime_error("CAN socket 创建失败: " + std::string(std::strerror(errno)));

    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, cfg_.interface.c_str(), IFNAMSIZ - 1);
    if (::ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        int e = errno; ::close(s);
        throw std::runtime_error("CAN 接口 " + cfg_.interface + " 不可用（是否已 ip link up？）: "
                                 + std::strerror(e));
    }

    if (cfg_.fd) {
        int on = 1;
        if (::setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on)) < 0) {
            int e = errno; ::close(s);
            throw std::runtime_error("接口 " + cfg_.interface + " 未启用 CAN FD: "
                                     + std::strerror(e));
        }
    }

    struct sockaddr_can addr {};
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno; ::close(s);
        throw std::runtime_error("bind " + cfg_.interface + " 失败: " + std::strerror(e));
    }

    sock_ = s;
    applyFilters();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        frames_.clear();
    }
    stop_      = false;
    link_down_ = false;
    reader_    = std::thread(&CanCollector::readerLoop, this);

    spdlog::info("CAN[{}] 已连接 (fd={}) 监听 {} 个信号",
                 cfg_.interface, cfg_.fd ? "on" : "off", cfg_.signals.size());
}

bool CanCollector::linkOk() const {
    return sock_ >= 0 && !link_down_.load(std::memory_order_relaxed);
}

// 只接收点表用到的 CAN ID，减少无关帧唤醒接收线程
void CanCollector::applyFilters() {
    if (cfg_.signals.empty()) return;   // 不设过滤器 = 全收

    std::vector<struct can_filter> filters;
    std::set<uint32_t> seen;
    for (const auto& s : cfg_.signals) {
        if (!seen.insert(slotKey(s.can_id, s.extended)).second) continue;
        struct can_filter f {};
        if (s.extended) {
            f.can_id   = (s.can_id & CAN_EFF_MASK) | CAN_EFF_FLAG;
            f.can_mask = CAN_EFF_MASK | CAN_EFF_FLAG;   // 掩码带 EFF 位 → 不与标准帧混淆
        } else {
            f.can_id   =  s.can_id & CAN_SFF_MASK;
            f.can_mask = CAN_SFF_MASK | CAN_EFF_FLAG;
        }
        filters.push_back(f);
    }

    if ((int)filters.size() > kMaxFilters) {
        spdlog::warn("CAN[{}] 唯一 ID {} 个超过内核上限 {}，改为接收全部帧",
                     cfg_.interface, filters.size(), kMaxFilters);
        return;
    }
    if (::setsockopt(sock_, SOL_CAN_RAW, CAN_RAW_FILTER, filters.data(),
                     filters.size() * sizeof(struct can_filter)) < 0)
        spdlog::warn("CAN[{}] 设置过滤器失败，改为接收全部帧: {}",
                     cfg_.interface, std::strerror(errno));
}

// ── 断开 ──────────────────────────────────────────────────────────────────────
void CanCollector::disconnect() {
    stop_ = true;
    if (reader_.joinable()) reader_.join();   // 最多等一个 POLL_TIMEOUT_MS
    if (sock_ >= 0) { ::close(sock_); sock_ = -1; }
    std::lock_guard<std::mutex> lk(mtx_);
    frames_.clear();
}

// ── 接收线程：按 CAN ID 只保留最新一帧 ────────────────────────────────────────
void CanCollector::readerLoop() {
    // canfd_frame 与 can_frame 的 can_id/len/data 字段偏移一致，
    // 因此同一缓冲区可接收两种帧，靠返回字节数区分。
    struct canfd_frame cf {};

    while (!stop_.load(std::memory_order_relaxed)) {
        struct pollfd pfd { sock_, POLLIN, 0 };
        int pr = ::poll(&pfd, 1, POLL_TIMEOUT_MS);
        if (pr == 0) continue;                       // 超时：回头检查 stop_
        if (pr < 0) {
            if (errno == EINTR) continue;
            spdlog::warn("CAN[{}] poll 失败: {}", cfg_.interface, std::strerror(errno));
            link_down_ = true;                       // 交给外层掉线重连
            return;
        }

        // 接口 down / 被删除时 poll 置 POLLERR，read 返回 ENETDOWN / ENODEV
        ssize_t n = ::read(sock_, &cf, sizeof(cf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            spdlog::warn("CAN[{}] read 失败: {}", cfg_.interface, std::strerror(errno));
            link_down_ = true;
            return;
        }
        if (n != (ssize_t)CAN_MTU && n != (ssize_t)CANFD_MTU) continue;
        if (cf.can_id & CAN_ERR_FLAG) continue;      // 错误帧不是数据
        if (n == (ssize_t)CAN_MTU && (cf.can_id & CAN_RTR_FLAG)) continue;  // 远程帧无负载

        const bool     eff = cf.can_id & CAN_EFF_FLAG;
        const uint32_t id  = cf.can_id & (eff ? CAN_EFF_MASK : CAN_SFF_MASK);
        int len = cf.len;
        if (len < 0)        len = 0;
        if (len > kMaxDlen) len = kMaxDlen;

        std::lock_guard<std::mutex> lk(mtx_);
        FrameSlot& slot = frames_[slotKey(id, eff)];
        std::memcpy(slot.data, cf.data, (size_t)len);
        slot.len = len;
        slot.ts  = std::chrono::steady_clock::now();
    }
}

// ── 解码 ──────────────────────────────────────────────────────────────────────
DataPoints CanCollector::readAll() {
    DataPoints out;
    out.reserve(cfg_.signals.size());

    const auto        now = std::chrono::steady_clock::now();
    const std::string ts  = nowIso();
    int good = 0;

    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& sig : cfg_.signals) {
            DataPoint dp;
            dp.name        = sig.name;
            dp.unit        = sig.unit;
            dp.description = sig.description;
            dp.timestamp   = ts;
            dp.quality     = Quality::BAD;   // 未收到 / 过期 / 位域越界

            auto it = frames_.find(slotKey(sig.can_id, sig.extended));
            if (it != frames_.end()) {
                const FrameSlot& slot = it->second;
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now - slot.ts).count();
                if (cfg_.stale_timeout_ms <= 0 || age <= cfg_.stale_timeout_ms) {
                    if (auto raw = canExtractRaw(slot.data, slot.len, sig)) {
                        dp.raw_value = static_cast<double>(*raw);
                        dp.value     = static_cast<double>(*raw) * sig.scale + sig.offset;
                        dp.quality   = Quality::GOOD;
                        ++good;
                    }
                }
            }
            out.push_back(std::move(dp));
        }
    }

    spdlog::debug("CAN[{}] {}/{} 信号 GOOD", cfg_.interface, good, out.size());
    return out;
}

} // namespace industrial
