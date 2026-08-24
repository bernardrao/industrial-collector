// test/compile_test.cpp — 「生效」编译器
//
// 判据是【每个实例算出的地址都对】，不是"编译成功"。地址算错不会崩溃，
// 只会静默读到别的寄存器 —— 所以断言逐个核对具体地址值，尤其是：
//   · 异构组合（7 簇 + 1 控制器）中控制器是否顶偏了后续簇的序号
//   · 节点覆盖是否真的压过了模型公式
//   · 地址撞车是否被报出来

#include "config_db.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <unistd.h>

using namespace industrial;
using json = nlohmann::json;

static int g_fail = 0;
#define CHECK(c,m) do{ if(c) printf("  ✓ %s\n",m); \
                       else {printf("  ✗ FAIL %s\n",m);++g_fail;} }while(0)

// 从编译结果里取某节点某测点的地址
static std::string addrOf(const std::vector<ConfigDb::CompiledRow>& rows,
                          const std::string& path, const std::string& key) {
    for (const auto& r : rows)
        if (r.node_path == path && r.point_key == key) return r.addr;
    return "(未找到)";
}

static void expectAddr(const std::vector<ConfigDb::CompiledRow>& rows,
                       const std::string& path, const std::string& key,
                       const std::string& want) {
    const std::string got = addrOf(rows, path, key);
    char msg[300];
    if (got == want) {
        snprintf(msg, sizeof msg, "%-34s %-8s → %s", path.c_str(), key.c_str(), want.c_str());
        printf("  ✓ %s\n", msg);
    } else {
        snprintf(msg, sizeof msg, "%s.%s 应为 %s，实得 %s",
                 path.c_str(), key.c_str(), want.c_str(), got.c_str());
        printf("  ✗ FAIL %s\n", msg); ++g_fail;
    }
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp";
    spdlog::set_level(spdlog::level::off);
    printf("══ 编译器（「生效」）══\n");

    const std::string dbp = dir + "/compiletest.db";
    ::unlink(dbp.c_str());
    ConfigDb db(dbp);
    if (!db.ok()) { printf("  ✗ 建库失败\n"); return 1; }

    // ── 搭一个缩小版储能站：2 簇 × 2 包 × 3 芯，外加一个控制器 ──────────────
    // 规模缩小但结构与真实一致（异构组合、多级序号），地址可以手工推导核对。
    printf("\n── 搭建：RACK = 2×CLST + 1×CTL，CLST = 2×PACK，PACK = 3×CELL ──\n");
    db.upsertModel({"CELL001","电芯","cell"});
    db.upsertModel({"PACK001","电池包","pack"});
    db.upsertModel({"CLST001","电池簇","cluster"});
    db.upsertModel({"CTL001","控制器","ctl"});
    db.upsertModel({"RACK001","电池舱","rack"});
    db.appendModelChild("PACK001","CELL001",3);
    db.appendModelChild("CLST001","PACK001",2);
    db.appendModelChild("RACK001","CLST001",2);
    db.appendModelChild("RACK001","CTL001",1);      // 异构：簇之后挂一个控制器

    db.upsertTask({"bms","modbus","{}",1000,true,5});

    // 电芯地址：簇跨 512，包跨 128，芯跨 2
    db.upsertBinding({"CELL001","voltage","bms",
                      "1000 + idx_CLST001*512 + idx_PACK001*128 + idx_CELL001*2",
                      "uint16",0.001,0.0,"{}"});
    db.upsertBinding({"CELL001","temp","bms",
                      "5000 + idx_CLST001*256 + idx_PACK001*64 + idx_CELL001",
                      "int16",0.1,0.0,"{}"});
    // 控制器：与电芯不同的地址段
    db.upsertBinding({"CTL001","run_state","bms","9000","uint16",1.0,0.0,"{}"});

    auto root = db.addNode(std::nullopt, "station", "电站", std::nullopt);
    int64_t created = 0; std::string ierr;
    bool inst = db.instantiateModel(root, "rack01", "1号舱", "RACK001", 10000, created, ierr);
    if (!inst) printf("    实例化失败: %s\n", ierr.c_str());
    CHECK(inst, "实例化 RACK001");
    // 1 舱 + 2 簇 + 4 包 + 12 芯 + 1 控制器 = 20
    CHECK(created == 20, "展开出 20 个节点（1舱+2簇+4包+12芯+1控制器）");

    // ── 编译 ──────────────────────────────────────────────────────────────
    printf("\n── 编译 ──\n");
    int64_t ver = 0;
    CompileStats st;
    std::vector<std::string> warns;
    std::string err;
    bool okc = compileTree(db, ver, st, warns, err);
    if (!okc) printf("    编译失败: %s\n", err.c_str());
    CHECK(okc, "编译成功");
    printf("    版本 %lld：节点 %lld / 测点 %lld（模型展开 %lld，节点覆盖 %lld），告警 %d\n",
           (long long)ver, (long long)st.nodes, (long long)st.points,
           (long long)st.from_model, (long long)st.overridden, st.warnings);
    // 12 芯 × 2 点 + 1 控制器 × 1 点 = 25
    CHECK(st.points == 25, "产出 25 个测点（12芯×2 + 控制器×1）");

    auto rows = db.compiledPoints(ver);
    CHECK((int64_t)rows.size() == st.points, "读回的编译点数与统计一致");

    // ── 逐个核对地址（手工推导）─────────────────────────────────────────
    printf("\n── 地址核对：1000 + 簇*512 + 包*128 + 芯*2 ──\n");
    // 第一个电芯 = 基址
    expectAddr(rows, "station.rack01.cluster1.pack1.cell1", "voltage", "1000");
    // 同包内第 2、3 芯
    expectAddr(rows, "station.rack01.cluster1.pack1.cell2", "voltage", "1002");
    expectAddr(rows, "station.rack01.cluster1.pack1.cell3", "voltage", "1004");
    // 同簇内第 2 个包
    expectAddr(rows, "station.rack01.cluster1.pack2.cell1", "voltage", "1128");
    // 第 2 个簇
    expectAddr(rows, "station.rack01.cluster2.pack1.cell1", "voltage", "1512");
    expectAddr(rows, "station.rack01.cluster2.pack2.cell3", "voltage",
               std::to_string(1000 + 1*512 + 1*128 + 2*2));
    // 第二个绑定用另一套系数，验证同节点多测点互不干扰
    expectAddr(rows, "station.rack01.cluster2.pack2.cell3", "temp",
               std::to_string(5000 + 1*256 + 1*64 + 2));

    // ── 异构组合：控制器不能顶偏簇的序号 ──────────────────────────────────
    printf("\n── 异构组合（2 簇 + 1 控制器）──\n");
    expectAddr(rows, "station.rack01.ctl1", "run_state", "9000");

    // rack01 的控制器排在两簇【之后】，此时按"全部兄弟"计数也得同样结果 ——
    // 这一例没有区分度。真正能分辨的是控制器排在【最前】：若按全部兄弟计数，
    // 控制器会占掉 idx=0，两个簇变成 1 和 2，地址整体偏移 512。
    // 故另建 RACK002 = 1×CTL + 2×CLST 专门验证。
    db.upsertModel({"RACK002","控制器在前的电池舱","rack"});
    db.appendModelChild("RACK002","CTL001",1);      // 控制器在前
    db.appendModelChild("RACK002","CLST001",2);
    {
        int64_t c2 = 0; std::string e2;
        CHECK(db.instantiateModel(root, "rack02", "2号舱", "RACK002", 10000, c2, e2),
              "实例化 RACK002（控制器在前）");
        int64_t v=0; CompileStats s; std::vector<std::string> w; std::string e;
        CHECK(compileTree(db, v, s, w, e), "重新编译");
        auto r = db.compiledPoints(v);
        // 控制器在前也不该影响簇的序号：第 1 簇仍是 idx=0 → 基址 1000
        expectAddr(r, "station.rack02.cluster1.pack1.cell1", "voltage", "1000");
        expectAddr(r, "station.rack02.cluster2.pack1.cell1", "voltage", "1512");
        CHECK(addrOf(r,"station.rack02.cluster1.pack1.cell1","voltage") == "1000",
              "控制器排在最前也未顶偏簇序号（按全部兄弟计数会算成 1512）");
    }

    // ── 节点覆盖优先于模型公式 ────────────────────────────────────────────
    printf("\n── 节点覆盖 ──\n");
    {
        auto n = db.nodeByPath("station.rack01.cluster1.pack1.cell2");
        CHECK(n.has_value(), "找到待覆盖的电芯节点");
        // 这一颗电芯的电压点改用手工地址（现场换了块表，地址不连续）
        db.addNodePoint({n->id,"voltage","bms","7777","uint16",0.001,0.0,
                         R"({"name":"voltage","address":7777})",0});
        int64_t v2=0; CompileStats s2; std::vector<std::string> w2; std::string e2;
        CHECK(compileTree(db, v2, s2, w2, e2), "覆盖后重新编译");
        auto r2 = db.compiledPoints(v2);
        expectAddr(r2, "station.rack01.cluster1.pack1.cell2", "voltage", "7777");
        // 同节点的另一个测点仍走模型公式
        expectAddr(r2, "station.rack01.cluster1.pack1.cell2", "temp",
                   std::to_string(5000 + 0*256 + 0*64 + 1));
        // 其余电芯不受影响
        expectAddr(r2, "station.rack01.cluster1.pack1.cell3", "voltage", "1004");
        CHECK(s2.overridden == 1, "统计里记了 1 个覆盖点");
    }

    // ── 地址撞车必须报告 ──────────────────────────────────────────────────
    printf("\n── 地址冲突检测 ──\n");
    {
        // 把 temp 的公式改成与 voltage 同段，制造大面积撞车
        db.upsertBinding({"CELL001","temp","bms",
                          "1000 + idx_CLST001*512 + idx_PACK001*128 + idx_CELL001*2",
                          "int16",0.1,0.0,"{}"});
        int64_t v3=0; CompileStats s3; std::vector<std::string> w3; std::string e3;
        CHECK(compileTree(db, v3, s3, w3, e3), "撞车不阻断编译（属告警非错误）");
        bool hasDup = std::any_of(w3.begin(), w3.end(), [](const std::string& s){
            return s.find("重复") != std::string::npos; });
        if (hasDup) printf("    示例告警: %s\n", w3.front().c_str());
        CHECK(hasDup, "地址重复被报出（否则现场会看到两个点读同一个值）");
        CHECK(s3.warnings > 0, "告警计数非零");
        // 恢复
        db.upsertBinding({"CELL001","temp","bms",
                          "5000 + idx_CLST001*256 + idx_PACK001*64 + idx_CELL001",
                          "int16",0.1,0.0,"{}"});
    }

    // ── 公式出错必须整体失败，不留半份快照 ────────────────────────────────
    printf("\n── 公式错误 → 整体回滚 ──\n");
    {
        const int64_t before = db.latestVersion();
        db.upsertBinding({"CELL001","voltage","bms","1000 + idx_NOSUCH*2",
                          "uint16",0.001,0.0,"{}"});
        int64_t v4=0; CompileStats s4; std::vector<std::string> w4; std::string e4;
        bool r = compileTree(db, v4, s4, w4, e4);
        CHECK(!r, "含未知变量的公式导致编译失败");
        if (!r) {
            bool named = e4.find("idx_NOSUCH") != std::string::npos &&
                         e4.find("cell") != std::string::npos;
            printf("    错误信息首行: %s\n", e4.substr(0, e4.find('\n')).c_str());
            CHECK(named, "错误指明了是哪个节点的哪个测点、哪个变量");
        }
        CHECK(db.latestVersion() == before, "失败的编译没有留下新版本（已回滚）");
    }

    // ── 版本保留 ──────────────────────────────────────────────────────────
    printf("\n── 版本 ──\n");
    CHECK(db.latestVersion() > 0, "存在编译版本");
    CHECK(db.countRows("config_versions") <= 10, "版本数受 pruneVersions(10) 约束");

    printf("\n════════════════════════════════════\n");
    if (g_fail) { printf("%d 项失败 ✗\n", g_fail); return 1; }
    printf("全部通过 ✓\n");
    return 0;
}
