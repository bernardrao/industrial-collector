#pragma once
/**
 * iec104_collector.h — IEC 60870-5-104 客户端（原生 POSIX socket）
 *
 * 支持 ASDU 类型：1(单点遥信) 3(双点) 11(规一化) 13(短浮点) 15(累计量)
 * 帧处理：STARTDT / TESTFR 心跳 / S帧确认 / 总召唤
 */
#include "types.h"
#include "config.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace industrial {

struct CachedValue { double value = 0.0; uint8_t flags = 0; };

class IEC104Collector {
public:
    explicit IEC104Collector(const IEC104Config& cfg);
    ~IEC104Collector();

    void       connect();
    void       disconnect();
    DataPoints readAll();

    double pollInterval() const { return cfg_.poll_interval; }

private:
    IEC104Config cfg_;
    int          sockfd_ = -1;

    std::thread       recvThread_;
    std::atomic<bool> running_{false};

    mutable std::mutex                   cacheMtx_;
    std::unordered_map<int, CachedValue> cache_;

    uint16_t sendSeq_ = 0;
    uint16_t recvSeq_ = 0;

    void sendStartDT();
    void sendGI();          // General Interrogation
    void sendSFrame();
    void sendTestFrCon();
    bool sendRaw(const uint8_t* data, size_t len);
    bool recvExact(uint8_t* buf, size_t len);

    void recvLoop();
    void parseAPDU(const uint8_t* body, size_t len);
    void parseASDU(const uint8_t* asdu, size_t len);

    struct ElemResult {
        double value = 0; uint8_t flags = 0;
        int consumed = 1; bool valid = false;
    };
    static ElemResult  decodeElement(uint8_t tid, const uint8_t* d, size_t avail);
    static double      applyScale(double raw, float scale, float offset);
    static Quality     flagsToQuality(uint8_t flags);
};

} // namespace industrial
