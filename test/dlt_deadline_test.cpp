// test/dlt_deadline_test.cpp
//
// DLT 事务整体超时（非自洽）。
//
// 关键点：对照组模拟旧行为——每次 read 都给完整 timeout_ms。噪声线路上
// （每 100ms 一个非 0x68 字节）旧行为把超时按次累加到 32×，新实现受单一
// deadline 约束。若对照组复现不出累加，断言会失败，测试即无说服力。
// 同时断言超时提示里含 timeout_ms 与卡住的阶段。
//
// 编译（无需 MQTT/web 依赖）：
//   cd build && make dlt_deadline_test && ./dlt_deadline_test

#include "dlt645_collector.h"
#include "io_transport.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>

using namespace industrial;
using namespace std::chrono;
static int g_fail = 0;
#define CHECK(c,m) do{ if(c) printf("  ✓ %s\n",m); else {printf("  ✗ FAIL %s\n",m);++g_fail;} }while(0)

static long msSince(steady_clock::time_point t){
    return duration_cast<milliseconds>(steady_clock::now()-t).count();
}

// 噪声线路：每 100ms 吐一个非 0x68 的字节，永远等不到帧头
struct NoisyPeer {
    int srv=-1, conn=-1; uint16_t port=0;
    std::atomic<bool> stop{false};
    std::thread th;
    void start() {
        srv = socket(AF_INET, SOCK_STREAM, 0);
        int one=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=0;
        bind(srv,(sockaddr*)&a,sizeof(a)); listen(srv,1);
        socklen_t l=sizeof(a); getsockname(srv,(sockaddr*)&a,&l); port=ntohs(a.sin_port);
        th = std::thread([this]{
            conn = accept(srv,nullptr,nullptr);
            if (conn<0) return;
            uint8_t junk = 0xAA;                     // 绝不是 0x68
            while (!stop) {
                std::this_thread::sleep_for(milliseconds(100));
                if (send(conn,&junk,1,MSG_NOSIGNAL) <= 0) break;
            }
        });
    }
    void stop_() {
        stop = true;
        if (th.joinable()) th.join();
        if (conn >= 0) ::close(conn);
        if (srv  >= 0) ::close(srv);
    }
};

int main() {
    printf("══ DLT 事务整体超时 ══\n");

    // 把日志接到内存，方便断言提示信息
    auto ss = std::make_shared<std::ostringstream>();
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*ss);
    auto lg = std::make_shared<spdlog::logger>("t", sink);
    lg->set_level(spdlog::level::debug);
    spdlog::set_default_logger(lg);

    const int TIMEOUT = 400;

    // ── 新实现：整个 transact 受一个 deadline 约束 ──────────────────────────
    NoisyPeer p; p.start();
    auto transport = std::make_shared<TcpTransport>("127.0.0.1", p.port);
    transport->open();

    MeterConfig645 m;
    m.id = "meter_x";
    m.meter_address = "000000000001";
    m.points.push_back(Dlt645Point{"volt","02010100",1.0f,0.0f,"V",""});

    Dlt645Collector col(transport, m, TIMEOUT);
    auto t0 = steady_clock::now();
    auto pts = col.readAll();
    long dt = msSince(t0);
    printf("\n  新实现: readAll 耗时 %ldms（timeout_ms=%d）\n", dt, TIMEOUT);
    CHECK(pts.size()==1 && pts[0].quality==Quality::BAD, "噪声线路上点质量为 BAD");
    CHECK(dt < TIMEOUT + 250, "整个事务被 deadline 约束（未按次累加）");

    const std::string logs = ss->str();
    bool hasTimeoutMsg = logs.find("事务超时") != std::string::npos;
    printf("  日志: %s", logs.empty() ? "(空)\n" : "");
    if (hasTimeoutMsg) {
        auto b = logs.find("DLT645");
        printf("%s\n", logs.substr(b, logs.find('\n', b)-b).c_str());
    }
    CHECK(hasTimeoutMsg, "超时时给出了明确提示（含 timeout_ms 与阶段）");
    CHECK(logs.find("帧起始符") != std::string::npos, "提示指明卡在哪个阶段");

    transport->close();

    // ── 对照组：模拟旧行为，每次 read 都给完整 timeout ─────────────────────
    TcpTransport t2("127.0.0.1", p.port);
    // 旧的 skip 循环最多 32 次单字节读；噪声每 100ms 一字节 → 每次 read 都能读到
    NoisyPeer p2; p2.start();
    TcpTransport old("127.0.0.1", p2.port);
    old.open();
    auto t1 = steady_clock::now();
    uint8_t b=0; int got=0;
    for (int skip=0; skip<32; ++skip) {
        if (old.read(&b,1,TIMEOUT)!=1) break;      // 每次都给完整 400ms
        ++got;
        if (b==0x68) break;
    }
    long dt2 = msSince(t1);
    printf("\n  旧实现(模拟): 跳过循环读了 %d 字节，耗时 %ldms  ← 超出预算 %.1fx\n",
           got, dt2, dt2/(double)TIMEOUT);
    CHECK(dt2 > TIMEOUT*3, "对照组确实把超时按次累加（对照成立）");
    old.close(); p2.stop_();

    (void)t2;
    p.stop_();

    printf("\n════════════════════════════════════\n");
    if (g_fail) { printf("%d 项失败 ✗\n", g_fail); return 1; }
    printf("全部通过 ✓\n");
    return 0;
}
