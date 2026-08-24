// test/can_signal_test.cpp
//
// CAN 信号位域解码测试（DBC 语义符合性，非自洽）。
//
// 关键点：期望值是【按 DBC 位编号规则手工推导的字面量】，不是用解码器自己算出来的
// ——因此能真正抓出 Intel/Motorola 位序颠倒、跨字节跳转错误、符号扩展缺失。
// canExtractRaw() 不依赖 socket，无需真实 CAN 接口即可运行。
//
// 编译：
//   cd build && make can_signal_test && ./can_signal_test

#include "can_collector.h"

#include <cstdio>
#include <cstdint>
#include <vector>

using namespace industrial;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ✓ %s\n", msg); } \
    else      { printf("  ✗ FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } \
} while (0)

// 构造信号：位序/长度/符号是被测维度，其余取默认
static CanSignal sig(int start_bit, int bit_length, CanByteOrder bo, bool is_signed) {
    CanSignal s;
    s.name       = "t";
    s.start_bit  = start_bit;
    s.bit_length = bit_length;
    s.byte_order = bo;
    s.is_signed  = is_signed;
    return s;
}

static bool eq(const std::vector<uint8_t>& data, const CanSignal& s, int64_t want) {
    auto got = canExtractRaw(data.data(), (int)data.size(), s);
    if (!got) { printf("    (返回 nullopt，期望 %lld)\n", (long long)want); return false; }
    if (*got != want) {
        printf("    (得到 %lld，期望 %lld)\n", (long long)*got, (long long)want);
        return false;
    }
    return true;
}
static bool isNull(const std::vector<uint8_t>& data, const CanSignal& s) {
    return !canExtractRaw(data.data(), (int)data.size(), s).has_value();
}

// ── Intel / little-endian ─────────────────────────────────────────────────────
static void test_little() {
    printf("\n── Intel (little-endian) ──\n");

    // start_bit 是 LSB，位号递增：byte0 为低字节
    CHECK(eq({0x34,0x12}, sig(0,16,CanByteOrder::LITTLE,false), 0x1234),
          "start=0 len=16  {34 12} → 0x1234");

    // 字节对齐的单字节信号
    CHECK(eq({0x34,0x12}, sig(8,8,CanByteOrder::LITTLE,false), 0x12),
          "start=8 len=8   {34 12} → 0x12");

    // 单比特：byte0 bit5
    CHECK(eq({0x20}, sig(5,1,CanByteOrder::LITTLE,false), 1),
          "start=5 len=1   {20} → 1");
    CHECK(eq({0x20}, sig(4,1,CanByteOrder::LITTLE,false), 0),
          "start=4 len=1   {20} → 0");

    // 非字节对齐、跨字节：bits 4..15 = byte0 高半字节 + byte1 全部
    // {0xAB,0xCD} → bit4..7 = 0xA, byte1 = 0xCD → 0xCDA
    CHECK(eq({0xAB,0xCD}, sig(4,12,CanByteOrder::LITTLE,false), 0xCDA),
          "start=4 len=12  {AB CD} → 0xCDA（跨字节）");
}

// ── Motorola / big-endian ────────────────────────────────────────────────────
static void test_big() {
    printf("\n── Motorola (big-endian) ──\n");

    // start_bit 是 MSB；byte0 bit7 起，字节内递减，到 bit0 跳下一字节 bit7
    CHECK(eq({0x12,0x34}, sig(7,16,CanByteOrder::BIG,false), 0x1234),
          "start=7 len=16  {12 34} → 0x1234");

    // 跨字节非对齐：byte0 低半字节 + byte1 高半字节 = 0xB,0xC → 0xBC
    CHECK(eq({0xAB,0xCD}, sig(3,8,CanByteOrder::BIG,false), 0xBC),
          "start=3 len=8   {AB CD} → 0xBC（跨字节跳转）");

    // 单比特：byte0 bit5（位号定义与 Intel 一致，只有取位方向不同）
    CHECK(eq({0x20}, sig(5,1,CanByteOrder::BIG,false), 1),
          "start=5 len=1   {20} → 1");

    // 同参数下两种位序必须给出不同结果 —— 证明 byte_order 真的生效
    // Motorola: byte0 全部(0x12) + byte1 bit7(0) = 0x24
    // Intel   : byte0 bit7(0) + byte1 全部(0x34) 左移 1 = 0x68
    CHECK(eq({0x12,0x34}, sig(7,9,CanByteOrder::BIG,false),    0x24),
          "start=7 len=9   {12 34} Motorola → 0x24");
    CHECK(eq({0x12,0x34}, sig(7,9,CanByteOrder::LITTLE,false), 0x68),
          "start=7 len=9   {12 34} Intel    → 0x68（与上式不同）");
}

// ── 符号扩展 ─────────────────────────────────────────────────────────────────
static void test_signed() {
    printf("\n── 符号扩展（二进制补码）──\n");

    CHECK(eq({0xFF,0xFF}, sig(0,16,CanByteOrder::LITTLE,true), -1),
          "len=16 全 1 → -1");
    CHECK(eq({0xFF,0xFF}, sig(0,16,CanByteOrder::LITTLE,false), 0xFFFF),
          "len=16 全 1 无符号 → 65535");

    // 12 位：raw=0xF00=3840，符号位(bit11)=1 → 3840-4096 = -256
    CHECK(eq({0x00,0x0F}, sig(0,12,CanByteOrder::LITTLE,true), -256),
          "len=12 raw=0xF00 → -256");

    // 正数不应被扩展
    CHECK(eq({0x00,0x07}, sig(0,12,CanByteOrder::LITTLE,true), 0x700),
          "len=12 raw=0x700（符号位0）→ 1792");

    // 64 位全 1：无需扩展，转型即 -1
    CHECK(eq({0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
             sig(0,64,CanByteOrder::LITTLE,true), -1),
          "len=64 全 1 → -1");

    // 单比特有符号：1 位补码的 1 表示 -1
    CHECK(eq({0x01}, sig(0,1,CanByteOrder::LITTLE,true), -1),
          "len=1 有符号 raw=1 → -1");
}

// ── CAN FD 长负载 ────────────────────────────────────────────────────────────
static void test_canfd() {
    printf("\n── CAN FD（>8 字节负载）──\n");

    std::vector<uint8_t> d(64, 0x00);
    d[63] = 0xAB;
    CHECK(eq(d, sig(504,8,CanByteOrder::LITTLE,false), 0xAB),
          "64 字节帧 start=504 len=8 → 0xAB（末字节）");

    d.assign(64, 0x00);
    d[32] = 0xFF; d[33] = 0x01;
    CHECK(eq(d, sig(256,9,CanByteOrder::LITTLE,false), 0x1FF),
          "64 字节帧 start=256 len=9 → 0x1FF");

    // 同一信号定义在 8 字节经典帧上越界
    CHECK(isNull({0,0,0,0,0,0,0,0}, sig(504,8,CanByteOrder::LITTLE,false)),
          "同信号用于 8 字节帧 → nullopt（越界）");
}

// ── 越界与非法参数 ───────────────────────────────────────────────────────────
static void test_bounds() {
    printf("\n── 越界 / 非法参数 ──\n");

    CHECK(isNull({0,0,0,0,0,0,0,0}, sig(60,16,CanByteOrder::LITTLE,false)),
          "Intel start=60 len=16 越过 64 位 → nullopt");
    CHECK(isNull({0x12}, sig(7,16,CanByteOrder::BIG,false)),
          "Motorola 单字节帧取 16 位 → nullopt");
    CHECK(isNull({0x12,0x34}, sig(0,0,CanByteOrder::LITTLE,false)),
          "bit_length=0 → nullopt");
    CHECK(isNull({0x12,0x34}, sig(0,65,CanByteOrder::LITTLE,false)),
          "bit_length=65 → nullopt");
    CHECK(isNull({0x12,0x34}, sig(-1,8,CanByteOrder::LITTLE,false)),
          "start_bit=-1 → nullopt");

    // 空帧（DLC=0）：任何信号都取不到
    CanSignal s = sig(0,8,CanByteOrder::LITTLE,false);
    CHECK(!canExtractRaw(nullptr, 0, s).has_value(), "data_len=0 → nullopt");
}

int main() {
    printf("══ CAN 信号解码测试（DBC 位编号符合性）══\n");
    test_little();
    test_big();
    test_signed();
    test_canfd();
    test_bounds();
    printf("\n════════════════════════════════════\n");
    if (g_fail == 0) { printf("全部通过 ✓\n"); return 0; }
    printf("%d 项失败 ✗\n", g_fail);
    return 1;
}
