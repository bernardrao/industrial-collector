#pragma once
/**
 * io_transport.h — 串口 / TCP 传输抽象
 * 供 DLT645 / DLT698 采集器共用；无外部依赖，使用 POSIX API。
 */
#include <chrono>
#include <cstdint>
#include <string>
#include <stdexcept>

namespace industrial {

// 距 deadline 还剩多少毫秒（不为负）。
// 【向上取整】：向下取整会在还剩 0.9ms 时就返回 0，让调用方白白提前放弃；
// 且多次调用累积的截断误差会使"预算是否耗尽"的判定变得不确定。
// 向上取整保证等待至少覆盖到 deadline，之后本函数必然返回 0。
int remainingMs(std::chrono::steady_clock::time_point deadline);

// ── 传输接口 ─────────────────────────────────────────────────────────────────
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual void open()  = 0;
    virtual void close() = 0;
    virtual int  write(const uint8_t* data, int len) = 0;
    // 返回实际读到的字节数；timeout_ms<=0 表示不等待
    virtual int  read(uint8_t* buf, int len, int timeout_ms) = 0;
    virtual bool isOpen() const = 0;
    // 清空接收缓冲区（发请求前调用）
    virtual void flush() = 0;
};

// ── 串口（RS485）──────────────────────────────────────────────────────────────
class SerialTransport : public ITransport {
public:
    SerialTransport(const std::string& port, int baud);
    ~SerialTransport() override { close(); }

    void open()  override;
    void close() override;
    int  write(const uint8_t* data, int len) override;
    int  read(uint8_t* buf, int len, int timeout_ms) override;
    bool isOpen() const override { return fd_ >= 0; }
    void flush() override;

private:
    std::string port_;
    int         baud_;
    int         fd_ = -1;

    static int baudConstant(int baud);
};

// ── TCP（RS485 转 TCP 网关）────────────────────────────────────────────────────
class TcpTransport : public ITransport {
public:
    TcpTransport(const std::string& host, int port);
    ~TcpTransport() override { close(); }

    void open()  override;
    void close() override;
    int  write(const uint8_t* data, int len) override;
    int  read(uint8_t* buf, int len, int timeout_ms) override;
    bool isOpen() const override { return fd_ >= 0; }
    void flush() override;

private:
    std::string host_;
    int         port_;
    int         fd_ = -1;
};

} // namespace industrial
