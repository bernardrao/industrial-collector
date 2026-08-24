// test/io_timeout_test.cpp
//
// io_transport 超时语义测试（非自洽）。
//
// 关键点：oldRead() 逐字复刻旧实现（select 的 tv 每轮重置），作为对照组。
// 若对照组不能复现"超时被放大"，本测试即无说服力 —— 断言里会检查这一点。
// 覆盖：整体截止时间、字节滴流、EINTR 不提前返回、串口路径(pty)、write/flush。
//
// 编译（无需 MQTT/web 依赖）：
//   cd build && make io_timeout_test && ./io_timeout_test

#include "io_transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pty.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace industrial;
using namespace std::chrono;
static int g_fail = 0;
#define CHECK(c,m) do{ if(c) printf("  ✓ %s\n",m); else {printf("  ✗ FAIL %s\n",m);++g_fail;} }while(0)

static long msSince(steady_clock::time_point t) {
    return duration_cast<milliseconds>(steady_clock::now() - t).count();
}

// ── 旧实现（逐字复刻）：每次循环把 select 超时重置为完整 timeout_ms ────────────
static int oldRead(int fd, uint8_t* buf, int len, int timeout_ms) {
    int total = 0;
    while (total < len) {
        fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
        struct timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        int r = select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) break;
        int n = (int)recv(fd, buf + total, len - total, 0);
        if (n <= 0) break;
        total += n;
    }
    return total;
}

// ── 测试用 TCP 回环：接受一个连接，按脚本发送字节 ────────────────────────────
struct Peer {
    int srv = -1, conn = -1;
    uint16_t port = 0;
    void listenAny() {
        srv = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(srv, (sockaddr*)&a, sizeof(a));
        listen(srv, 1);
        socklen_t l = sizeof(a); getsockname(srv, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
    }
    void acceptOne() { conn = accept(srv, nullptr, nullptr); }
    void close_() { if (conn>=0) ::close(conn); if (srv>=0) ::close(srv); }
};

// ── 1. 无数据：耗时应恰好 = timeout，而非 N × timeout ────────────────────────
static void test_no_data() {
    printf("\n── 无数据可读，请求 9 字节，timeout=300ms ──\n");
    Peer p; p.listenAny();
    std::thread th([&]{ p.acceptOne(); std::this_thread::sleep_for(milliseconds(4000)); });

    TcpTransport t("127.0.0.1", p.port); t.open();
    std::this_thread::sleep_for(milliseconds(50));

    uint8_t buf[9];
    auto t0 = steady_clock::now();
    int n = t.read(buf, 9, 300);
    long dt = msSince(t0);
    printf("    新实现: 返回 %d 字节，耗时 %ldms\n", n, dt);
    CHECK(n == 0 && dt >= 250 && dt < 600, "新实现 ≈ 300ms（整体截止）");

    // 对照：旧实现在同一 socket 上
    int raw = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons(p.port);
    (void)connect(raw, (sockaddr*)&a, sizeof(a));
    auto t1 = steady_clock::now();
    int n2 = oldRead(raw, buf, 9, 300);
    long dt2 = msSince(t1);
    printf("    旧实现: 返回 %d 字节，耗时 %ldms\n", n2, dt2);
    // 无数据时旧实现第一次 select 就超时 break，所以这里两者相同 —— 差异在下一个用例
    CHECK(dt2 >= 250 && dt2 < 600, "旧实现此场景也是 300ms（差异需靠字节滴流暴露）");
    ::close(raw);
    th.join(); t.close(); p.close_();
}

// ── 2. 字节滴流：这才是超时被放大的场景 ──────────────────────────────────────
static void test_trickle() {
    printf("\n── 字节滴流：每 150ms 来 1 字节，请求 6 字节，timeout=400ms ──\n");
    printf("   预算 400ms 内最多只能收到 ~2-3 字节；旧实现会一直读到 6 字节（~900ms）\n");

    // 新实现
    {
        Peer p; p.listenAny();
        std::atomic<bool> stop{false};
        std::thread th([&]{
            p.acceptOne();
            for (int i = 0; i < 6 && !stop; ++i) {
                std::this_thread::sleep_for(milliseconds(150));
                uint8_t b = (uint8_t)i; send(p.conn, &b, 1, 0);
            }
        });
        TcpTransport t("127.0.0.1", p.port); t.open();
        uint8_t buf[6];
        auto t0 = steady_clock::now();
        int n = t.read(buf, 6, 400);
        long dt = msSince(t0);
        printf("    新实现: 返回 %d 字节，耗时 %ldms\n", n, dt);
        CHECK(dt < 600, "新实现遵守 400ms 总预算");
        CHECK(n < 6, "新实现在预算内只读到部分字节（符合截止语义）");
        stop = true; th.join(); t.close(); p.close_();
    }
    // 旧实现
    {
        Peer p; p.listenAny();
        std::atomic<bool> stop{false};
        std::thread th([&]{
            p.acceptOne();
            for (int i = 0; i < 6 && !stop; ++i) {
                std::this_thread::sleep_for(milliseconds(150));
                uint8_t b = (uint8_t)i; send(p.conn, &b, 1, 0);
            }
        });
        int raw = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons(p.port);
        (void)connect(raw, (sockaddr*)&a, sizeof(a));
        uint8_t buf[6];
        auto t0 = steady_clock::now();
        int n = oldRead(raw, buf, 6, 400);
        long dt = msSince(t0);
        printf("    旧实现: 返回 %d 字节，耗时 %ldms  ← 超出 400ms 预算 %.1fx\n",
               n, dt, dt / 400.0);
        CHECK(dt > 700, "旧实现确实把超时放大到远超预算（对照组成立）");
        stop = true; th.join(); ::close(raw); p.close_();
    }
}

// ── 3. EINTR 不应被当成超时 ─────────────────────────────────────────────────
static volatile sig_atomic_t g_sig = 0;
static void onAlarm(int){ ++g_sig; }

static void test_eintr() {
    printf("\n── 信号打断（EINTR）：不应提前返回 ──\n");
    Peer p; p.listenAny();
    std::thread th([&]{ p.acceptOne(); std::this_thread::sleep_for(milliseconds(2000)); });
    TcpTransport t("127.0.0.1", p.port); t.open();
    std::this_thread::sleep_for(milliseconds(30));

    struct sigaction sa{};
    sa.sa_handler = onAlarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                 // 不设 SA_RESTART → poll/select 返回 EINTR
    sigaction(SIGALRM, &sa, nullptr);

    itimerval it{};
    it.it_value.tv_usec = 100000;    // 100ms 后打断一次
    setitimer(ITIMER_REAL, &it, nullptr);

    uint8_t buf[4];
    auto t0 = steady_clock::now();
    int n = t.read(buf, 4, 700);
    long dt = msSince(t0);
    printf("    信号触发 %d 次；返回 %d 字节，耗时 %ldms\n", (int)g_sig, n, dt);
    CHECK(g_sig >= 1, "确实收到了 SIGALRM");
    CHECK(dt >= 600, "被 EINTR 打断后继续等到截止时间（旧实现会在 ~100ms 就返回）");

    signal(SIGALRM, SIG_IGN);
    th.join(); t.close(); p.close_();
}

// ── 4. 串口路径（用 pty 模拟）───────────────────────────────────────────────
static void test_serial_pty() {
    printf("\n── 串口路径（openpty 模拟）──\n");
    int master = -1, slave = -1;
    char name[128];
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0) {
        printf("  (openpty 不可用，跳过)\n"); return;
    }
    ::close(slave);

    SerialTransport s(name, 9600);
    s.open();
    CHECK(s.isOpen(), "pty 作为串口成功打开");

    // 无数据：耗时 ≈ timeout
    uint8_t buf[8];
    auto t0 = steady_clock::now();
    int n = s.read(buf, 8, 250);
    long dt = msSince(t0);
    printf("    无数据读 8 字节: 返回 %d，耗时 %ldms\n", n, dt);
    CHECK(n == 0 && dt >= 200 && dt < 500, "串口无数据 ≈ 250ms（非 8 × 250ms）");

    // 有数据：立刻返回
    const char* msg = "ABCD";
    if (::write(master, msg, 4) != 4) printf("    [warn] pty write 失败\n");
    std::this_thread::sleep_for(milliseconds(30));
    auto t1 = steady_clock::now();
    int n2 = s.read(buf, 4, 500);
    long dt2 = msSince(t1);
    printf("    有数据读 4 字节: 返回 %d，耗时 %ldms\n", n2, dt2);
    CHECK(n2 == 4 && dt2 < 100, "数据就绪时立即返回");
    CHECK(memcmp(buf, msg, 4) == 0, "数据内容正确");

    // timeout<=0 = 不等待，但仍取走已就绪数据
    if (::write(master, "XY", 2) != 2) printf("    [warn] pty write 失败\n");
    std::this_thread::sleep_for(milliseconds(30));
    int n3 = s.read(buf, 2, 0);
    CHECK(n3 == 2 && buf[0]=='X' && buf[1]=='Y', "timeout=0 仍取走已就绪数据");

    auto t2 = steady_clock::now();
    int n4 = s.read(buf, 2, 0);
    CHECK(n4 == 0 && msSince(t2) < 50, "timeout=0 且无数据 → 立即返回 0");

    s.close(); ::close(master);
}

// ── 5. write / flush 回归（这两处也换成了 poll）──────────────────────────────
static void test_write_flush() {
    printf("\n── write / flush 回归 ──\n");
    Peer p; p.listenAny();
    std::thread th([&]{
        p.acceptOne();
        uint8_t b[16];
        int n = (int)recv(p.conn, b, 5, 0);          // 收 5 字节
        if (n == 5) send(p.conn, b, 5, 0);           // 原样回送
        std::this_thread::sleep_for(milliseconds(80));
        send(p.conn, (const void*)"GARBAGE", 7, 0);  // 制造待丢弃的残余
        std::this_thread::sleep_for(milliseconds(600));
    });
    TcpTransport t("127.0.0.1", p.port); t.open();

    const uint8_t out[5] = {1,2,3,4,5};
    CHECK(t.write(out, 5) == 5, "write 返回写入字节数");

    uint8_t in[5];
    CHECK(t.read(in, 5, 500) == 5 && memcmp(in, out, 5) == 0, "回环数据正确");

    std::this_thread::sleep_for(milliseconds(200));   // 等 GARBAGE 到达
    t.flush();
    uint8_t junk[8];
    auto t0 = steady_clock::now();
    int n = t.read(junk, 1, 100);
    CHECK(n == 0 && msSince(t0) >= 80, "flush 已丢弃残余数据（再读只剩超时）");

    th.join(); t.close(); p.close_();
}

int main() {
    printf("══ io_transport 超时语义测试 ══\n");
    test_no_data();
    test_trickle();
    test_eintr();
    test_serial_pty();
    test_write_flush();
    printf("\n════════════════════════════════════\n");
    if (g_fail) { printf("%d 项失败 ✗\n", g_fail); return 1; }
    printf("全部通过 ✓\n");
    return 0;
}
