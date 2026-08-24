// src/io_transport.cpp — 串口 / TCP 传输实现（POSIX，无外部依赖）
#include "io_transport.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

// ── POSIX 头文件 ──────────────────────────────────────────────────────────────
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

namespace industrial {

// 剩余毫秒数，向上取整（见头文件注释）
int remainingMs(std::chrono::steady_clock::time_point deadline) {
    using namespace std::chrono;
    auto ns = duration_cast<nanoseconds>(deadline - steady_clock::now()).count();
    if (ns <= 0) return 0;
    return (int)((ns + 999999) / 1000000);
}

namespace {

// 读满 len 字节，或到达【整体】截止时间为止，返回实际读到的字节数。
//
// 旧实现每次循环都把 select 的 timeout 重置为完整的 timeout_ms，于是"读 N 字节"
// 最坏要等 N × timeout_ms（DLT645 读 9 字节帧头、timeout_ms=3000 → 27 秒）。
// 这里改为一次调用共享一个截止时间。
//
// 另外两处修正：
//   * EINTR 不再被当成超时 —— 旧代码 select 返回 -1 就 break，丢掉已读字节；
//   * 用 poll 而非 select —— fd >= FD_SETSIZE(1024) 时 FD_SET 是未定义行为。
//
// timeout_ms <= 0 表示"不等待"：仍会做一次立即返回的 poll，把已就绪的数据取走。
int readDeadline(int fd, uint8_t* buf, int len, int timeout_ms, bool socketMode) {
    if (fd < 0)   return -1;
    if (len <= 0) return 0;

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    int total = 0;
    while (total < len) {
        struct pollfd p { fd, POLLIN, 0 };
        int r = ::poll(&p, 1, remainingMs(deadline));
        if (r < 0) {
            if (errno == EINTR) continue;   // 被信号打断，不消耗剩余预算
            break;
        }
        if (r == 0) break;                  // 到达截止时间

        int n = socketMode ? (int)::recv(fd, buf + total, len - total, 0)
                           : (int)::read(fd, buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        if (n == 0) break;                  // 对端关闭 / EOF
        total += n;
    }
    return total;
}

// 写满 len 字节；EINTR 重试。返回 -1 表示真失败。
int writeAll(int fd, const uint8_t* data, int len, bool socketMode) {
    if (fd < 0) return -1;
    int total = 0;
    while (total < len) {
        int n = socketMode ? (int)::send(fd, data + total, len - total, MSG_NOSIGNAL)
                           : (int)::write(fd, data + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            // 串口以 O_NONBLOCK 打开：内核 tty 缓冲写满时会 EAGAIN，等可写再续
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd p { fd, POLLOUT, 0 };
                if (::poll(&p, 1, 1000) > 0) continue;
            }
            return -1;
        }
        total += n;
    }
    return total;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// SerialTransport
// ═════════════════════════════════════════════════════════════════════════════

SerialTransport::SerialTransport(const std::string& port, int baud)
    : port_(port), baud_(baud) {}

int SerialTransport::baudConstant(int baud) {
    switch (baud) {
        case   1200: return B1200;
        case   2400: return B2400;
        case   4800: return B4800;
        case   9600: return B9600;
        case  19200: return B19200;
        case  38400: return B38400;
        case  57600: return B57600;
        case 115200: return B115200;
        default:     return B9600;
    }
}

void SerialTransport::open() {
    fd_ = ::open(port_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0)
        throw std::runtime_error("无法打开串口 " + port_ + ": " + strerror(errno));

    struct termios tty{};
    if (tcgetattr(fd_, &tty) != 0) {
        ::close(fd_); fd_ = -1;
        throw std::runtime_error("tcgetattr 失败: " + std::string(strerror(errno)));
    }

    cfsetispeed(&tty, baudConstant(baud_));
    cfsetospeed(&tty, baudConstant(baud_));

    // 8N1 原始模式
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |=  (CS8 | CREAD | CLOCAL);
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;  // 非阻塞，超时由 readDeadline() 的 poll 控制

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        ::close(fd_); fd_ = -1;
        throw std::runtime_error("tcsetattr 失败: " + std::string(strerror(errno)));
    }
    tcflush(fd_, TCIOFLUSH);
}

void SerialTransport::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

void SerialTransport::flush() {
    if (fd_ >= 0) tcflush(fd_, TCIFLUSH);
}

int SerialTransport::write(const uint8_t* data, int len) {
    int total = writeAll(fd_, data, len, /*socketMode=*/false);
    if (total > 0) tcdrain(fd_);  // 等待发送完毕（RS485 半双工需要）
    return total;
}

int SerialTransport::read(uint8_t* buf, int len, int timeout_ms) {
    return readDeadline(fd_, buf, len, timeout_ms, /*socketMode=*/false);
}

// ═════════════════════════════════════════════════════════════════════════════
// TcpTransport
// ═════════════════════════════════════════════════════════════════════════════

TcpTransport::TcpTransport(const std::string& host, int port)
    : host_(host), port_(port) {}

void TcpTransport::open() {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
        throw std::runtime_error("DNS 解析失败: " + host_);

    fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_ < 0) { freeaddrinfo(res); throw std::runtime_error("socket() 失败"); }

    // 非阻塞 connect + select 超时（5s）
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd_, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) {
        ::close(fd_); fd_ = -1;
        throw std::runtime_error("TCP 连接失败 " + host_ + ":" + portStr);
    }
    if (rc != 0) {
        struct pollfd p { fd_, POLLOUT, 0 };
        int pr;
        do { pr = ::poll(&p, 1, 5000); } while (pr < 0 && errno == EINTR);
        if (pr <= 0) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("TCP 连接超时 " + host_ + ":" + portStr);
        }
        int err = 0; socklen_t el = sizeof(err);
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("TCP 连接错误: " + std::string(strerror(err)));
        }
    }
    // 恢复阻塞 + 禁用 Nagle
    fcntl(fd_, F_SETFL, flags);
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

void TcpTransport::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

void TcpTransport::flush() {
    if (fd_ < 0) return;
    // 丢弃 socket 接收缓冲区中的残余数据（poll 超时 0 = 立即返回）
    uint8_t tmp[64];
    struct pollfd p { fd_, POLLIN, 0 };
    while (::poll(&p, 1, 0) > 0) {
        if (recv(fd_, tmp, sizeof(tmp), 0) <= 0) break;
        p.revents = 0;
    }
}

int TcpTransport::write(const uint8_t* data, int len) {
    return writeAll(fd_, data, len, /*socketMode=*/true);
}

int TcpTransport::read(uint8_t* buf, int len, int timeout_ms) {
    return readDeadline(fd_, buf, len, timeout_ms, /*socketMode=*/true);
}

} // namespace industrial
