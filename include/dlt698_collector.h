#pragma once
/**
 * dlt698_collector.h — DL/T 698.45 单表采集器（测试版）
 * transport 由总线线程注入，多表共享同一连接，顺序访问。
 */
#include "types.h"
#include "io_transport.h"
#include <memory>
#include <vector>
#include <cstdint>
#include <string>

namespace industrial {

class Dlt698Collector {
public:
    Dlt698Collector(std::shared_ptr<ITransport> transport,
                    const MeterConfig698& meter,
                    int timeout_ms);

    DataPoints  readAll();
    const std::string& meterId() const { return meter_.id; }

private:
    std::shared_ptr<ITransport> transport_;
    MeterConfig698               meter_;
    int                          timeout_ms_;
    uint8_t                      addr_[6]{};
    uint8_t                      ser_ = 0;

    static uint16_t crc16(const uint8_t* data, int len);
    std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& inner);
    bool                 sendConnect();
    std::vector<uint8_t> buildGetRequest(const uint8_t oad[4]);
    std::vector<uint8_t> transact(const std::vector<uint8_t>& frame);
    bool parseGetResponse(const std::vector<uint8_t>& appdu, double& out);
    bool decodeTypedData(const uint8_t* p, int len, double& out);
    DataPoint readPoint(const Dlt698Point& pt);
};

} // namespace industrial
