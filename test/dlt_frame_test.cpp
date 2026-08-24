// test/dlt_frame_test.cpp
//
// DL/T 645 / 698 字节级单元测试（规约符合性，非自洽）。
//
// 关键点：期望帧字节是【按规约规则手工推导的字面量】，不是用采集器自己的
// 编码函数生成的——因此能真正抓出 +0x33 偏移、BCD 字节序、校验和/CRC 错误。
// 通过 Mock transport 注入采集器：捕获其发出的请求字节，并回灌手工构造的
// 响应帧，验证编码与解码两个方向。
//
// 编译（无需 MQTT/web 依赖）：
//   g++-9 -std=c++17 -Iinclude -Ithird_party/spdlog/include \
//       test/dlt_frame_test.cpp src/dlt645_collector.cpp \
//       src/dlt698_collector.cpp src/io_transport.cpp -o /tmp/dlt_test -pthread
//   /tmp/dlt_test
//
// 或： cd build && make dlt_frame_test && ./dlt_frame_test

#include "dlt645_collector.h"
#include "dlt698_collector.h"
#include "io_transport.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <string>

using namespace industrial;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ✓ %s\n", msg); } \
    else      { printf("  ✗ FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } \
} while (0)

static std::string hex(const std::vector<uint8_t>& v) {
    std::string s;
    char b[4];
    for (auto x : v) { snprintf(b, sizeof(b), "%02X ", x); s += b; }
    return s;
}

// ── Mock transport：捕获写出字节，按队列回放响应 ──────────────────────────────
class MockTransport : public ITransport {
public:
    std::vector<uint8_t> written;       // 采集器发出的所有字节
    std::deque<uint8_t>  to_read;       // 预置的响应字节，按序供 read()

    void open()  override {}
    void close() override {}
    bool isOpen() const override { return true; }
    void flush() override {}

    int write(const uint8_t* d, int n) override {
        written.insert(written.end(), d, d + n);
        return n;
    }
    int read(uint8_t* buf, int n, int) override {
        int i = 0;
        for (; i < n && !to_read.empty(); ++i) { buf[i] = to_read.front(); to_read.pop_front(); }
        return i;
    }
    void preload(const std::vector<uint8_t>& v) {
        for (auto x : v) to_read.push_back(x);
    }
};

// ═══════════════════════ 独立 CRC-16/ARC 实现（带标准向量自检）═══════════════
static uint16_t crc16_arc(const std::vector<uint8_t>& d) {
    uint16_t crc = 0x0000;
    for (auto byte : d) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

// ═══════════════════════════════ DL/T 645 ════════════════════════════════════
static void test_dlt645() {
    printf("[DL/T 645-2007]\n");

    // 表地址 "123456789012"，读 DI=00010000（正向有功总电能）
    MeterConfig645 m;
    m.id = "t1";
    m.meter_address = "123456789012";
    Dlt645Point pt;
    pt.name = "energy"; pt.data_id = "00010000"; pt.scale = 0.01f;
    m.points.push_back(pt);

    auto mock = std::make_shared<MockTransport>();

    // ── 手工按规约推导的期望请求帧 ──────────────────────────────────────────
    // 唤醒 FE×4 | 68 | 地址(LSB-first BCD) | 68 | 11 | 04 | DI(LSB,+33) | CS | 16
    //   地址自然序 12 34 56 78 90 12 → 帧内逆序: 12 90 78 56 34 12
    //   DI=0x00010000，低字节在前传输 → 字节序 00 00 01 00 → 各+0x33 = 33 33 34 33
    //   CS = (68+12+90+78+56+34+12+68+11+04+33+33+34+33) & FF = 68
    std::vector<uint8_t> expect_req = {
        0xFE,0xFE,0xFE,0xFE,
        0x68, 0x12,0x90,0x78,0x56,0x34,0x12, 0x68,
        0x11, 0x04, 0x33,0x33,0x34,0x33,
        0x68, 0x16
    };

    // ── 手工构造的响应帧（值=12345，4字节BCD LSB-first 45 23 01 00）──────────
    //   68 | 地址 | 68 | 91 | 08 | DI(+33) | 值(+33) | CS | 16
    //   DI 同请求 00 00 01 00 → +0x33 = 33 33 34 33
    //   值字节 45 23 01 00 → 各+0x33 = 78 56 34 33
    //   CS = (68+12+90+78+56+34+12+68+91+08+33+33+34+33+78+56+34+33)&FF = 21
    std::vector<uint8_t> resp = {
        0x68, 0x12,0x90,0x78,0x56,0x34,0x12, 0x68,
        0x91, 0x08, 0x33,0x33,0x34,0x33, 0x78,0x56,0x34,0x33,
        0x21, 0x16
    };
    mock->preload(resp);

    Dlt645Collector col(mock, m, 1000);
    auto pts = col.readAll();

    CHECK(mock->written == expect_req,
          "请求帧字节与规约推导一致（+0x33/LSB-BCD/校验和）");
    if (mock->written != expect_req) {
        printf("    期望: %s\n    实际: %s\n",
               hex(expect_req).c_str(), hex(mock->written).c_str());
    }

    CHECK(pts.size() == 1 && pts[0].quality == Quality::GOOD, "响应被接受（校验和通过）");
    double v = pts.size() ? pts[0].toDouble().value_or(-1) : -1;
    CHECK(pts.size() && std::abs(v - 123.45) < 1e-3, "解码值 = 123.45 (12345 × 0.01)");
    if (pts.size()) printf("    解码值 = %.4f\n", v);
}

// ═══════════════════════════════ DL/T 698.45 ═════════════════════════════════
static void test_dlt698() {
    printf("[DL/T 698.45]\n");

    // CRC-16/ARC 标准向量自检（独立锚点）："123456789" → 0xBB3D
    std::vector<uint8_t> vec = {'1','2','3','4','5','6','7','8','9'};
    CHECK(crc16_arc(vec) == 0xBB3D, "CRC-16/ARC 标准向量 \"123456789\" = 0xBB3D");

    MeterConfig698 m;
    m.id = "s1";
    m.meter_address = "000000000001";
    Dlt698Point pt;
    pt.name = "volt"; pt.oad = "02010100"; pt.scale = 1.0f;
    m.points.push_back(pt);

    auto mock = std::make_shared<MockTransport>();

    // ── 手工构造 GET 响应：APPDU = 81 PIID OAD(02 01 01 00) 00 06 <float32 LE 220.1>
    float fval = 220.1f;
    uint8_t fb[4]; std::memcpy(fb, &fval, 4);
    std::vector<uint8_t> appdu = {0x81, 0x00, 0x02,0x01,0x01,0x00, 0x00, 0x06,
                                  fb[0],fb[1],fb[2],fb[3]};
    // INNER = CTRL(00) SER(00) DSALEN(06) DSA[6] SSALEN(06) SSA[6] APPDU
    std::vector<uint8_t> inner = {0x00,0x00, 0x06,0,0,0,0,0,0, 0x06,0,0,0,0,0,0};
    inner.insert(inner.end(), appdu.begin(), appdu.end());

    uint16_t len = (uint16_t)inner.size();
    std::vector<uint8_t> crc_in = {(uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    crc_in.insert(crc_in.end(), inner.begin(), inner.end());
    uint16_t crc = crc16_arc(crc_in);

    std::vector<uint8_t> frame = {0x68, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    frame.insert(frame.end(), inner.begin(), inner.end());
    frame.push_back(crc & 0xFF); frame.push_back(crc >> 8); frame.push_back(0x16);
    mock->preload(frame);

    Dlt698Collector col(mock, m, 1000);
    auto pts = col.readAll();

    // 请求帧基本结构校验：起始 0x68、长度域、CRC、结尾 0x16
    CHECK(!mock->written.empty() && mock->written.front() == 0x68, "GET 请求以 0x68 起始");
    bool req_crc_ok = false;
    if (mock->written.size() >= 6) {
        const auto& w = mock->written;
        uint16_t wl = w[1] | (w[2] << 8);
        if (w.size() == (size_t)wl + 6 && w.back() == 0x16) {
            std::vector<uint8_t> ci(w.begin()+1, w.begin()+3+wl);
            uint16_t wc = crc16_arc(ci);
            uint16_t got = w[3+wl] | (w[4+wl] << 8);
            req_crc_ok = (wc == got);
        }
    }
    CHECK(req_crc_ok, "GET 请求 CRC-16 正确（独立校验）");

    CHECK(pts.size() == 1 && pts[0].quality == Quality::GOOD, "GET 响应被接受（CRC 通过）");
    double v = pts.size() ? pts[0].toDouble().value_or(-1) : -1;
    CHECK(pts.size() && std::abs(v - 220.1) < 1e-2, "解码 float32 = 220.1");
    if (pts.size()) printf("    解码值 = %.4f\n", v);
}

int main() {
    printf("══ DLT 帧字节级单元测试（规约符合性）══\n");
    test_dlt645();
    test_dlt698();
    printf("════════════════════════════════════\n");
    if (g_fail == 0) { printf("全部通过 ✓\n"); return 0; }
    printf("%d 项失败 ✗\n", g_fail);
    return 1;
}
