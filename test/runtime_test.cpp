// test/runtime_test.cpp — 编译快照 → 运行时设备表
//
// 这一步是 P2 里风险最高的：还原错了，采集器会去读错误的地址，而且不会崩溃，
// 只会读回一堆看似合理的数。所以判据是【逐条核对】：
//   · 还原出的测点数与编译快照一致
//   · 地址逐个对得上（尤其是编译期算出的公式结果，不能被 raw_json 里的旧值盖掉）
//   · 迁移来的老设备测点名保持原样（否则升级后 MQTT 主题全变，下游订阅全断）
//   · DLT 的两级结构（总线 → 电表）能正确还原，meter_address 不丢
//   · 还原结果能被既有的 appConfigFromJsonString 吃下去（否则等于没用）

#include "config.h"
#include "config_db.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unistd.h>

using namespace industrial;
using json = nlohmann::json;

static int g_fail = 0;
#define CHECK(c,m) do{ if(c) printf("  ✓ %s\n",m); \
                       else {printf("  ✗ FAIL %s\n",m);++g_fail;} }while(0)

int main(int argc, char** argv) {
    const std::string cfgPath = argc > 1 ? argv[1] : "config/config.json";
    const std::string dir     = argc > 2 ? argv[2] : "/tmp";
    spdlog::set_level(spdlog::level::off);
    printf("══ 编译快照 → 运行时设备表 ══\n");

    const std::string dbp = dir + "/runtimetest.db";
    ::unlink(dbp.c_str());
    ConfigDb db(dbp);
    if (!db.ok()) { printf("  ✗ 建库失败\n"); return 1; }

    // ── 用真实 config.json 建库（含七协议：modbus/iec104/61850/opcua/dlt×2/can）──
    printf("\n── 准备：迁入真实配置 ──\n");
    {
        AppConfig a;
        try { a = loadConfig(cfgPath); }
        catch (const std::exception& e) { printf("  ✗ 读配置失败: %s\n", e.what()); return 1; }
        MigrateStats ms; std::string e;
        CHECK(migrateJsonToDb(appConfigToJsonString(a), db, ms, e), "迁入配置库");
        printf("    任务 %d / 节点 %d / 测点 %d\n", ms.tasks, ms.nodes, ms.points);
    }

    // 迁移来的设备是自由节点，测点已带字面地址，无需绑定即可编译
    printf("\n── 编译 ──\n");
    int64_t ver = 0; CompileStats cs; std::vector<std::string> warns; std::string cerr;
    bool okc = compileTree(db, ver, cs, warns, cerr);
    if (!okc) printf("    编译失败: %s\n", cerr.c_str());
    CHECK(okc, "编译成功");
    printf("    版本 %lld，测点 %lld\n", (long long)ver, (long long)cs.points);

    // ── 还原运行时设备表 ──────────────────────────────────────────────────
    printf("\n── 还原 ──\n");
    std::string rjson; RuntimeStats rs; std::string rerr;
    bool okr = buildRuntimeJson(db, ver, rjson, rs, rerr);
    if (!okr) printf("    还原失败: %s\n", rerr.c_str());
    CHECK(okr, "还原成功");
    printf("    任务 %lld / 测点 %lld / 电表 %lld（禁用 %lld，空 %lld）\n",
           (long long)rs.tasks, (long long)rs.points, (long long)rs.meters,
           (long long)rs.disabled_tasks, (long long)rs.empty_tasks);

    // 编译器对【禁用的任务也编译】—— 这样启用一个任务不必重新「生效」，
    // 由运行时按 tasks.enabled 过滤。故基准是"启用任务的测点数"而非全部。
    int64_t expEnabled = 0;
    {
        std::map<std::string,bool> en;
        for (const auto& t : db.listTasks()) en[t.id] = t.enabled;
        for (const auto& r : db.compiledPoints(ver)) if (en[r.task]) ++expEnabled;
    }
    printf("    快照中属于启用任务的测点: %lld\n", (long long)expEnabled);
    CHECK(rs.points == expEnabled, "还原的测点数 = 快照中启用任务的测点数（无丢失）");

    // ── 关键：还原结果必须能被既有解析器吃下 ──────────────────────────────
    printf("\n── 既有解析器能否接受 ──\n");
    AppConfig rc;
    bool parsed = true;
    try { rc = appConfigFromJsonString(rjson); }
    catch (const std::exception& e) {
        parsed = false;
        printf("    解析失败: %s\n", e.what());
    }
    CHECK(parsed, "appConfigFromJsonString 能解析还原结果（复用七个采集器的前提）");

    // 逐设备统计解析后的测点数，与快照比对
    if (parsed) {
        int64_t total = 0;
        for (const auto& d : rc.devices) {
            switch (d.protocol) {
                case Protocol::MODBUS:   total += (int64_t)d.modbus.points.size(); break;
                case Protocol::IEC104:   total += (int64_t)d.iec104.points.size(); break;
                case Protocol::IEC61850: total += (int64_t)d.iec61850.points.size(); break;
                case Protocol::OPCUA:    total += (int64_t)d.opcua.points.size(); break;
                case Protocol::CAN:      total += (int64_t)d.can.signals.size(); break;
                case Protocol::DLT645:
                    for (const auto& m : d.dlt645.meters) total += (int64_t)m.points.size();
                    break;
                case Protocol::DLT698:
                    for (const auto& m : d.dlt698.meters) total += (int64_t)m.points.size();
                    break;
            }
        }
        printf("    解析后共 %lld 个测点\n", (long long)total);
        CHECK(total == expEnabled, "解析后的测点总数仍与快照一致（协议块结构正确）");
    }

    // ── 地址逐条核对 ──────────────────────────────────────────────────────
    // 编译期算出的地址是权威值；raw_json 里可能残留展开前的旧值，绝不能盖过它。
    printf("\n── 地址逐条核对 ──\n");
    {
        auto rows = db.compiledPoints(ver);
        std::map<std::string,std::string> want;   // "task|name" → addr
        for (const auto& r : rows) {
            std::string nm = r.point_key;
            if (r.node_path != r.task) {
                std::string rel = r.node_path;
                if (rel.rfind(r.task + ".", 0) == 0) rel = rel.substr(r.task.size() + 1);
                // DLT 的表内测点名保持原样，其余带节点前缀
                nm = (r.task == "rs485_bus_1" || r.task == "tcp_gw_meters")
                         ? r.point_key : rel + "/" + r.point_key;
            }
            want[r.task + "|" + nm] = r.addr;
        }

        json J = json::parse(rjson);
        int checked = 0, wrong = 0;
        for (const auto& d : J["devices"]) {
            const std::string task = d["id"];
            const std::string proto = d["protocol"];
            auto visit = [&](const json& pts) {
                for (const auto& p : pts) {
                    const std::string nm = p.value("name","");
                    auto it = want.find(task + "|" + nm);
                    if (it == want.end()) return;    // 名字规则差异，另有断言覆盖
                    // 取该点实际的地址字段
                    std::string got;
                    for (auto k : {"address","ioa","object_ref","node_id",
                                   "data_id","oad","can_id"})
                        if (p.contains(k)) {
                            const auto& v = p[k];
                            got = v.is_string() ? v.get<std::string>()
                                : v.is_number_integer() ? std::to_string(v.get<int64_t>())
                                : v.dump();
                            break;
                        }
                    ++checked;
                    if (got != it->second) {
                        if (wrong < 3)
                            printf("    ✗ %s.%s 快照=%s 还原=%s\n",
                                   task.c_str(), nm.c_str(), it->second.c_str(), got.c_str());
                        ++wrong;
                    }
                }
            };
            const json& blk = d[proto];
            if (blk.contains("points"))  visit(blk["points"]);
            if (blk.contains("signals")) visit(blk["signals"]);
            if (blk.contains("meters"))
                for (const auto& m : blk["meters"])
                    if (m.contains("points")) visit(m["points"]);
        }
        printf("    核对了 %d 个地址\n", checked);
        CHECK(checked > 0, "确实核对到了地址（不是空跑）");
        CHECK(wrong == 0, "每个地址都与编译快照一致");
    }

    // ── 老设备的测点名不能变 ──────────────────────────────────────────────
    // 迁移来的扁平设备，节点路径就等于任务 id，测点名必须保持原样，
    // 否则升级后 MQTT 主题全变，下游订阅全断。
    printf("\n── 兼容性：老设备测点名保持原样 ──\n");
    if (parsed) {
        bool found = false, kept = false;
        for (const auto& d : rc.devices)
            if (d.id == "plc_line1")
                for (const auto& p : d.modbus.points) {
                    found = true;
                    if (p.name == "temperature") kept = true;
                }
        CHECK(found, "找到迁移来的 plc_line1");
        CHECK(kept, "测点名仍是 \"temperature\"，未变成 \"plc_line1/temperature\"");
    }

    // ── DLT 两级结构 ──────────────────────────────────────────────────────
    printf("\n── DLT 总线 → 电表 两级还原 ──\n");
    if (parsed) {
        for (const auto& d : rc.devices) {
            if (d.protocol != Protocol::DLT645) continue;
            printf("    %s: %zu 块表\n", d.id.c_str(), d.dlt645.meters.size());
            CHECK(d.dlt645.meters.size() == 2, "rs485_bus_1 还原出 2 块表");
            bool addrOk = true, idOk = true;
            for (const auto& m : d.dlt645.meters) {
                if (m.meter_address.empty()) addrOk = false;
                if (m.id.empty()) idOk = false;
            }
            CHECK(idOk, "每块表的 id 都在");
            CHECK(addrOk, "每块表的 meter_address 都在（来自节点 meta，不丢）");
            break;
        }
    }

    // ── 禁用的任务不该进运行时 ────────────────────────────────────────────
    printf("\n── 禁用任务 ──\n");
    CHECK(rs.disabled_tasks > 0, "确实存在被禁用的任务（源配置有 4 个 disabled）");
    if (parsed) {
        bool anyDisabled = false;
        for (const auto& d : rc.devices)
            if (d.id == "bms_can" || d.id == "scada_server") anyDisabled = true;
        CHECK(!anyDisabled, "禁用的任务未出现在运行时设备表里");
    }

    // ── 设备规格驱动的点位：scale / offset / data_type 必须落到运行时 ──────
    // 上面用的是迁移来的配置，raw_json 里本就带着 scale/data_type，所以【测不出】
    // 这个缺陷。设备规格驱动的点位不一样：raw_json 是编译器现造的，只有
    // name/address/_formula/_node，scale 与 dtype 只存在于 compiled_points 的列里。
    // 不把列写回去，解析器就取默认值 —— 一万多个点全 GOOD、界面一片绿，
    // 而电芯电压显示 3307 而不是 3.307，int16 的负电流变成 6 万多的正数。
    printf("\n── 设备规格驱动：scale / dtype 落地 ──\n");
    {
        const std::string dbM = dir + "/rt_model.db";
        ::unlink(dbM.c_str());
        ConfigDb db2(dbM);
        std::string e2;

        TaskRow t; t.id = "bms"; t.protocol = "modbus"; t.interval_ms = 1000;
        t.endpoint_json = R"({"host":"127.0.0.1","port":1502,"unit_id":1})";
        CHECK(db2.upsertTask(t), "建 modbus 任务");
        CHECK(db2.upsertModel({"CELL", "电芯", "cell"}), "建设备规格");
        CHECK(db2.addModelPoint({"CELL","voltage","单体电压","V",0}), "规格点位 voltage");
        CHECK(db2.addModelPoint({"CELL","temp","单体温度","℃",1}),   "规格点位 temp");

        BindingRow b1; b1.model="CELL"; b1.point_key="voltage"; b1.task="bms";
        b1.addr_formula="10000 + gidx_CELL*2"; b1.dtype="uint16"; b1.scale=0.001; b1.offset=0;
        BindingRow b2; b2.model="CELL"; b2.point_key="temp"; b2.task="bms";
        b2.addr_formula="40000 + gidx_CELL";   b2.dtype="int16";  b2.scale=0.1;   b2.offset=0;
        CHECK(db2.upsertBinding(b1) && db2.upsertBinding(b2), "两条地址绑定");

        auto root = db2.addNode(std::nullopt, "bms", "电池组", std::nullopt);
        CHECK(root.has_value(), "建根节点");
        int n_inst = 0;
        for (int i = 1; i <= 3 && root; ++i) {
            char code[16]; std::snprintf(code, sizeof(code), "cell%02d", i);
            if (db2.addNode(*root, code, code, std::string("CELL"))) ++n_inst;
        }
        CHECK(n_inst == 3, "实例化 3 个电芯");

        int64_t ver = 0; CompileStats cst; std::vector<std::string> w;
        CHECK(compileTree(db2, ver, cst, w, e2), "编译");

        std::string rj; RuntimeStats rst2;
        CHECK(buildRuntimeJson(db2, ver, rj, rst2, e2), "构建运行配置");

        // ── 对照组：编译器存进 raw_json 的那份，本身确实没有 scale/data_type ──
        {
            auto rows = db2.compiledPoints(ver, "bms");
            bool anyRaw = false, rawHasScale = false, rawHasDtype = false;
            for (const auto& r : rows) {
                anyRaw = true;
                json rr = json::parse(r.raw_json);
                if (rr.contains("scale"))     rawHasScale = true;
                if (rr.contains("data_type")) rawHasDtype = true;
            }
            CHECK(anyRaw, "快照里有点位");
            CHECK(!rawHasScale && !rawHasDtype,
                  "对照组复现：编译器的 raw_json 里没有 scale / data_type（列才是权威）");
        }

        AppConfig rc2;
        bool ok2 = true;
        try { rc2 = appConfigFromJsonString(rj); }
        catch (const std::exception& ex) { ok2 = false; printf("    解析失败: %s\n", ex.what()); }
        CHECK(ok2, "运行时解析器能吃下");

        if (ok2) {
            const ModbusConfig* mb = nullptr;
            for (const auto& d : rc2.devices) if (d.id == "bms") mb = &d.modbus;
            CHECK(mb != nullptr, "运行时有 bms 设备");
            if (mb) {
                CHECK((int)mb->points.size() == 6, "3 个电芯 × 2 点 = 6 个测点");
                int vOk = 0, tOk = 0;
                for (const auto& p : mb->points) {
                    if (p.name.find("voltage") != std::string::npos) {
                        if (std::abs(p.scale - 0.001f) < 1e-9f &&
                            p.data_type == ModbusDataType::UINT16) ++vOk;
                    } else if (p.name.find("temp") != std::string::npos) {
                        // int16 是判据的关键：按 uint16 解的话负温度会变成 6 万多
                        if (std::abs(p.scale - 0.1f) < 1e-9f &&
                            p.data_type == ModbusDataType::INT16) ++tOk;
                    }
                }
                CHECK(vOk == 3, "3 个 voltage 的 scale=0.001 且 uint16");
                CHECK(tOk == 3, "3 个 temp 的 scale=0.1 且【int16】（原 bug 会退化成 uint16）");
            }
        }
    }

    printf("\n════════════════════════════════════\n");
    if (g_fail) { printf("%d 项失败 ✗\n", g_fail); return 1; }
    printf("全部通过 ✓\n");
    return 0;
}
