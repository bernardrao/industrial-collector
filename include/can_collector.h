#pragma once
/**
 * can_collector.h — CAN / CAN FD 采集器（Linux SocketCAN，无第三方依赖）
 *
 * CAN 是广播总线，没有请求/应答语义，因此不走 ITransport（面向字节流）。
 * connect() 打开 AF_CAN RAW socket 并启动接收线程，按 CAN ID 缓存最新一帧；
 * readAll() 从缓存解码全部信号，对外呈现与其他采集器一致的轮询接口，
 * 从而直接复用 main.cpp 的 runSingleLoop / 掉线重连逻辑。
 *
 * 掉线判定与轮询型协议不同：总线静默（全 BAD）只说明没有目标帧在广播，不是链路故障，
 * 重开 socket 也变不出数据。故 runSingleLoop 对本采集器改用 linkOk()：
 * 只有接收线程因 socket 报错退出时才触发重连。实测接口被 down 掉或删除，
 * poll() 会置 POLLERR 且 read() 返回 ENETDOWN / ENODEV，两种真实故障均可捕获。
 */
#include "types.h"
#include "config.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace industrial {

// 按 DBC 语义从帧数据中提取信号原始值（is_signed 时做符号扩展）。
// data_len 为帧有效字节数；位域越界或 bit_length 非法时返回 nullopt。
// 独立于 socket，便于单元测试（见 test/can_signal_test.cpp）。
//
// 精度：上报管道是 double，故 bit_length > 53 的信号会丢低位；且无符号 64 位信号
// 取值 ≥ 2^63 时按有符号回绕。CAN 实际信号极少超过 32 位，此处不额外处理。
std::optional<int64_t> canExtractRaw(const uint8_t* data, int data_len,
                                     const CanSignal& sig);

class CanCollector {
public:
    explicit CanCollector(const CanConfig& cfg);
    ~CanCollector();
    CanCollector(const CanCollector&)            = delete;
    CanCollector& operator=(const CanCollector&) = delete;

    void       connect();      // 打开 socket + 启动接收线程；失败抛异常
    void       disconnect();   // 幂等
    DataPoints readAll();      // 从帧缓存解码，不阻塞总线

    // 链路是否可用。false = socket 已报错（接口 down/被删除），接收线程已退出，
    // 需要外层重连。总线上没有帧不影响此值。
    bool linkOk() const;

    double pollInterval() const { return cfg_.poll_interval; }

private:
    static constexpr int kMaxDlen = 64;            // CANFD_MAX_DLEN
    static constexpr int kMaxFilters = 512;        // 内核 CAN_RAW_FILTER_MAX

    struct FrameSlot {
        uint8_t data[kMaxDlen]{};
        int     len = 0;
        std::chrono::steady_clock::time_point ts{};
    };

    CanConfig          cfg_;
    int                sock_ = -1;
    std::thread        reader_;
    std::atomic<bool>  stop_{false};
    std::atomic<bool>  link_down_{false};          // 接收线程因 socket 错误退出时置位
    std::mutex         mtx_;                       // 保护 frames_
    std::unordered_map<uint32_t, FrameSlot> frames_;

    void readerLoop();
    void applyFilters();

    // 标准帧 0x123 与扩展帧 0x123 是不同的帧，键里必须带上区分位
    static uint32_t slotKey(uint32_t can_id, bool extended);
};

} // namespace industrial
