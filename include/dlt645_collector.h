#pragma once
/**
 * dlt645_collector.h — DL/T 645-2007 单表采集器
 *
 * transport 由调用方（总线线程）创建并共享，同一串口/TCP 连接上的多块表
 * 依次调用各自的 Dlt645Collector::readAll()，串行执行保证帧不冲突。
 */
#include "types.h"
#include "io_transport.h"
#include <memory>
#include <vector>
#include <cstdint>
#include <string>

namespace industrial {

class Dlt645Collector {
public:
    // transport: 由总线线程统一 open/close，此处只借用
    Dlt645Collector(std::shared_ptr<ITransport> transport,
                    const MeterConfig645& meter,
                    int timeout_ms);

    DataPoints  readAll();
    const std::string& meterId() const { return meter_.id; }

private:
    std::shared_ptr<ITransport> transport_;
    MeterConfig645               meter_;
    int                          timeout_ms_;
    uint8_t                      addr_[6]{};   // LSB-first BCD

    bool parseMeterAddr(const std::string& s, uint8_t out[6]);
    std::vector<uint8_t> buildReadFrame(uint32_t di_val);
    std::vector<uint8_t> transact(const std::vector<uint8_t>& req);
    static double bcdToDouble(const uint8_t* bytes, int len);
    DataPoint readPoint(const Dlt645Point& pt);
};

} // namespace industrial
