// test/config_db_test.cpp — 配置库 schema + 迁移器
//
// 核心是【语义往返】：真实 config.json → 迁移入库 → 导出 → 与原文逐字段比对。
// 迁移器的典型失效不是"插入行数不对"，而是【静默丢字段】—— 七个协议块、四种
// 点位容器形状，每一种都是一次丢字段的机会。所以断言不是"迁移成功"，而是
// "导出结果与原文完全相等"，并在不等时精确指出是哪条 JSON 路径丢了或变了。

#include "config.h"
#include "config_db.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

using namespace industrial;
using json = nlohmann::json;

static const char* CSV_HEADER_PROBE = "node_path,node_name,model,point_key";

static int g_fail = 0;
#define CHECK(c,m) do{ if(c) printf("  ✓ %s\n",m); \
                       else {printf("  ✗ FAIL %s\n",m);++g_fail;} }while(0)

// ── JSON 递归比对：报出每一处差异的完整路径 ─────────────────────────────────
static void diffJson(const json& a, const json& b, const std::string& path,
                     std::vector<std::string>& out, int limit = 25) {
    if ((int)out.size() >= limit) return;

    if (a.type() != b.type()) {
        out.push_back(path + ": 类型不同 (" + std::string(a.type_name()) +
                      " vs " + std::string(b.type_name()) + ")");
        return;
    }
    if (a.is_object()) {
        for (auto it = a.begin(); it != a.end(); ++it) {
            if (!b.contains(it.key())) out.push_back(path + "." + it.key() + ": 导出后丢失");
            else diffJson(it.value(), b[it.key()], path + "." + it.key(), out, limit);
        }
        for (auto it = b.begin(); it != b.end(); ++it)
            if (!a.contains(it.key())) out.push_back(path + "." + it.key() + ": 导出后多出");
        return;
    }
    if (a.is_array()) {
        if (a.size() != b.size()) {
            out.push_back(path + ": 数组长度 " + std::to_string(a.size()) +
                          " → " + std::to_string(b.size()));
            return;
        }
        for (size_t i = 0; i < a.size(); ++i)
            diffJson(a[i], b[i], path + "[" + std::to_string(i) + "]", out, limit);
        return;
    }
    if (a != b)
        out.push_back(path + ": " + a.dump() + " → " + b.dump());
}

int main(int argc, char** argv) {
    const std::string cfgPath = argc > 1 ? argv[1] : "config/config.json";
    const std::string dbDir   = argc > 2 ? argv[2] : "/tmp";

    spdlog::set_level(spdlog::level::warn);   // 只看测试输出
    printf("══ config.db 存储层 + 迁移器 ══\n");

    // ── 1. 语义往返（本测试的核心）────────────────────────────────────────
    printf("\n── 语义往返：config.json → config.db → config.json ──\n");
    std::string canonical;
    // 期望值从输入配置推导，不写死 —— 换任何配置文件跑都成立
    size_t expTasks = 0, expNodes = 0, expPoints = 0;
    {
        AppConfig a;
        try { a = loadConfig(cfgPath); }
        catch (const std::exception& e) {
            printf("  ✗ FAIL 读取 %s 失败: %s\n", cfgPath.c_str(), e.what());
            return 1;
        }
        // 以 appConfigToJsonString 的规范形式为基准：协议简写已归一、默认值已补全，
        // 迁移器面对的是规范形式，比对也就不受用户手写风格影响。
        canonical = appConfigToJsonString(a);

        json A = json::parse(canonical);
        size_t disabled = 0;
        for (const auto& d : A["devices"]) {
            ++expTasks;
            ++expNodes;                       // 每个设备一个根节点
            if (!d.value("enabled", true)) ++disabled;
            const std::string proto = d["protocol"];
            const json& blk = d[proto];
            if (blk.contains("meters")) {
                for (const auto& m : blk["meters"]) {
                    ++expNodes;               // 每块电表一个子节点
                    if (m.contains("points")) expPoints += m["points"].size();
                }
            } else {
                for (auto c : {"points","signals"})
                    if (blk.contains(c)) expPoints += blk[c].size();
            }
        }
        printf("    源配置: %zu 个设备（其中 %zu 个 disabled）\n", expTasks, disabled);
        CHECK(expTasks > 0, "loadConfig 载入了设备（disabled 的也在内，不被过滤）");
    }

    const std::string db1 = dbDir + "/cfgtest1.db";
    ::unlink(db1.c_str());
    {
        ConfigDb db(db1);
        CHECK(db.ok(), "配置库创建成功");

        MigrateStats st;
        std::string err;
        bool okMig = migrateJsonToDb(canonical, db, st, err);
        if (!okMig) printf("    迁移错误: %s\n", err.c_str());
        CHECK(okMig, "迁移执行成功");
        printf("    迁移统计: 任务 %d / 节点 %d / 点位 %d / 通道 %d / 设置 %d\n",
               st.tasks, st.nodes, st.points, st.channels, st.settings);
        CHECK((size_t)st.tasks  == expTasks,  "每个设备迁移为一个采集任务");
        CHECK((size_t)st.nodes  == expNodes,  "设备节点 + 电表子节点数量吻合");
        CHECK((size_t)st.points == expPoints, "点位一条不多、一条不少");

        std::string exported;
        bool okExp = exportDbToJson(db, exported, err);
        if (!okExp) printf("    导出错误: %s\n", err.c_str());
        CHECK(okExp, "导出执行成功");

        // —— 真正的判据 ——
        // 只比对 config.db 承载的部分：采集侧（devices）。
        // web / logging / cache / mqtt 留在 config.json —— 前三者是系统设置需人工
        // 可编辑，mqtt 则是为避免"系统设置页与通道页两个写入口"（见 config_migrate.cpp）。
        json Afull = json::parse(canonical), Bfull = json::parse(exported);
        json A = json::object(), B = json::object();
        for (auto k : {"devices"}) {
            if (Afull.contains(k)) A[k] = Afull[k];
            if (Bfull.contains(k)) B[k] = Bfull[k];
        }
        CHECK(!Bfull.contains("web") && !Bfull.contains("logging") &&
              !Bfull.contains("cache") && !Bfull.contains("mqtt"),
              "系统设置与 MQTT 未被卷入配置库（仍归 config.json，单一写入口）");
        std::vector<std::string> d;
        diffJson(A, B, "", d);
        if (!d.empty()) {
            printf("    ── 差异 %zu 处 ──\n", d.size());
            for (auto& s : d) printf("      %s\n", s.c_str());
        }
        CHECK(d.empty(), "往返后 JSON 逐字段完全相等（无字段丢失/变形）");
    }

    // ── 1b. 空容器：刚建好、还没添子项的设备 ──────────────────────────────
    // Web UI 必然造出这种中间状态（建了总线还没加表）。容器键若靠"有没有子节点"
    // 反推，这里就会丢 meters 键、凭空多出 points 键。
    printf("\n── 空 meters 的 DLT 总线（容器键必须由协议决定）──\n");
    {
        json e = json::parse(canonical);
        // 造一个空总线；若源配置无 DLT 设备则跳过
        bool found = false;
        for (auto& d : e["devices"]) {
            const std::string p = d["protocol"];
            if (p == "dlt645" || p == "dlt698") {
                d[p]["meters"] = json::array();
                e["devices"] = json::array({d});
                found = true;
                break;
            }
        }
        if (!found) {
            printf("    (源配置无 DLT 设备，跳过)\n");
        } else {
            const std::string dbE = dbDir + "/cfgtest_empty.db";
            ::unlink(dbE.c_str());
            ConfigDb db(dbE);
            MigrateStats st; std::string err;
            CHECK(migrateJsonToDb(e.dump(), db, st, err), "空总线迁移成功");
            std::string out;
            CHECK(exportDbToJson(db, out, err), "空总线导出成功");
            json Bf = json::parse(out);
            json eD = json::object(), BD = json::object();
            eD["devices"] = e["devices"];
            if (Bf.contains("devices")) BD["devices"] = Bf["devices"];
            std::vector<std::string> d2;
            diffJson(eD, BD, "", d2);
            for (auto& s : d2) printf("      %s\n", s.c_str());
            CHECK(d2.empty(), "空 meters 往返后仍是空 meters（未丢键、未多出 points）");
        }
    }

    // ── 2. 外键约束真的生效（PRAGMA foreign_keys 默认是关的）──────────────
    printf("\n── 外键约束 ──\n");
    {
        const std::string db2 = dbDir + "/cfgtest2.db";
        ::unlink(db2.c_str());
        ConfigDb db(db2);

        db.upsertModel({"CELL001", "电芯", "cell"});
        auto root = db.addNode(std::nullopt, "station", "电站", std::nullopt);
        CHECK(root.has_value(), "根节点创建成功");

        // 引用不存在的设备规格必须被外键挡下
        auto bad = db.addNode(root, "c1", "", std::string("NO_SUCH_MODEL"));
        CHECK(!bad.has_value(), "引用不存在的设备规格被外键拒绝（未开 PRAGMA 则会放行）");

        auto good = db.addNode(root, "c1", "", std::string("CELL001"));
        CHECK(good.has_value(), "引用已存在的设备规格可以创建");

        db.addNodePoint({*good, "v", "t1", "1000", "uint16", 0.1, 0.0, "{}", 0});
        CHECK(db.countRows("tree_nodes") == 2 && db.countRows("node_points") == 1,
              "删除前：2 个节点、1 个点位");

        // 模型正被节点引用 → RESTRICT 挡下，避免设备树指向不存在的模型
        CHECK(!db.deleteModel("CELL001"), "删除被实例化的设备规格被 RESTRICT 拒绝");

        // 删根节点 → 子节点与其点位应一并消失（CASCADE）
        CHECK(db.deleteNode(*root), "删除根节点成功");
        CHECK(db.countRows("tree_nodes") == 0, "子节点被级联删除");
        CHECK(db.countRows("node_points") == 0, "子节点的点位被级联删除（无孤儿行）");

        // 引用没了，此时才允许删模型
        CHECK(db.deleteModel("CELL001"), "无引用后可以删除设备规格");
    }

    // ── 3. 组合成环必须被拒绝（编译器展开子树时假定 DAG）──────────────────
    printf("\n── 规格组合环检测 ──\n");
    {
        const std::string db3 = dbDir + "/cfgtest3.db";
        ::unlink(db3.c_str());
        ConfigDb db(db3);

        db.upsertModel({"CELL001","电芯","cell"});
        db.upsertModel({"PACK001","电池包","pack"});
        db.upsertModel({"CLST001","电池簇","cluster"});

        CHECK(db.addModelChild({"PACK001","CELL001",24,0}), "PACK001 ← 24×CELL001");
        CHECK(db.addModelChild({"CLST001","PACK001",4,0}),  "CLST001 ← 4×PACK001");

        CHECK(!db.addModelChild({"CELL001","CELL001",1,0}), "自环被拒绝");
        // CELL001 → CLST001 会成环：CLST→PACK→CELL→CLST
        CHECK(!db.addModelChild({"CELL001","CLST001",1,0}), "间接环 CELL→CLST 被拒绝");
        CHECK(db.childrenOf("CELL001").empty(), "被拒绝的组合确实没写进库");
    }

    // ── 4. path 派生与节点身份 ────────────────────────────────────────────
    printf("\n── 设备树 path 派生 ──\n");
    {
        const std::string db4 = dbDir + "/cfgtest4.db";
        ::unlink(db4.c_str());
        ConfigDb db(db4);

        auto st   = db.addNode(std::nullopt, "station", "电站", std::nullopt);
        auto rack = db.addNode(st,   "rack01", "1号舱", std::nullopt);
        auto clst = db.addNode(rack, "clst03", "3号簇", std::nullopt);
        CHECK(st && rack && clst, "三层节点创建成功");

        auto n = db.nodeByPath("station.rack01.clst03");
        CHECK(n.has_value(), "按 path 能查到深层节点");
        CHECK(n && n->code == "clst03", "path 末段与 code 一致");

        // code 含 '.' 会让 path 无法反解，必须拒绝
        auto badCode = db.addNode(st, "a.b", "", std::nullopt);
        CHECK(!badCode.has_value(), "含 '.' 的 code 被拒绝（否则 path 无法反解）");

        // path 唯一：同一父下同名节点应失败
        auto dup = db.addNode(st, "rack01", "", std::nullopt);
        CHECK(!dup.has_value(), "同父同名节点被 path UNIQUE 拒绝");
    }

    // ── 4b. 改名 / 移动：子树 path 重写，引用不受影响 ─────────────────────
    // 这是当初选代理主键而非以 path 为身份的全部理由，必须验证。
    printf("\n── 节点改名 / 移动 ──\n");
    {
        const std::string db6 = dbDir + "/cfgtest6.db";
        ::unlink(db6.c_str());
        ConfigDb db(db6);

        auto st   = db.addNode(std::nullopt, "station", "电站", std::nullopt);
        auto rack = db.addNode(st,   "rack01", "1号舱", std::nullopt);
        auto clst = db.addNode(rack, "clst01", "1号簇", std::nullopt);
        auto pack = db.addNode(clst, "pack01", "1号包", std::nullopt);
        db.addNodePoint({*pack, "v", "t1", "1000", "uint16", 0.1, 0.0, "{}", 0});

        CHECK(db.renameNode(*rack, "rackA"), "改名成功");
        auto deep = db.nodeByPath("station.rackA.clst01.pack01");
        CHECK(deep.has_value(), "整棵子树的 path 都被重写");
        CHECK(!db.nodeByPath("station.rack01.clst01.pack01").has_value(),
              "旧 path 不再存在");
        // 关键：点位靠 node_id 引用，改名后仍挂在原节点上
        CHECK(deep && db.nodePoints(deep->id).size() == 1,
              "改名后点位仍在（引用指向 id，不受 path 变动影响）");
        CHECK(deep && deep->id == *pack, "节点 id 未变（代理主键的意义）");

        // 移动：把 clst01 挪到 station 下
        CHECK(db.moveNode(*clst, st), "移动成功");
        CHECK(db.nodeByPath("station.clst01.pack01").has_value(), "移动后子树 path 正确");
        CHECK(!db.nodeByPath("station.rackA.clst01").has_value(), "旧位置已无该节点");

        // 不许移到自己的子孙下（否则子树脱离根成环）
        CHECK(!db.moveNode(*clst, pack), "移到自己的子孙下被拒绝");
        CHECK(!db.moveNode(*clst, clst), "移到自己下面被拒绝");

        // code 含 LIKE 通配符时也不能误伤：'%' 若不转义会匹配到无关节点
        auto w1 = db.addNode(st, "a%b", "", std::nullopt);
        auto w2 = db.addNode(st, "axb", "", std::nullopt);
        CHECK(w1 && w2, "含 '%' 与相近名的兄弟节点都能建立");
        db.addNode(w1, "child", "", std::nullopt);
        CHECK(db.renameNode(*w1, "a%c"), "含 '%' 的节点改名成功");
        CHECK(db.nodeByPath("station.a%c.child").has_value(), "其子节点 path 已重写");
        CHECK(db.nodeByPath("station.axb").has_value(),
              "相近名的兄弟未被误伤（用 substr 而非 LIKE 定位子树）");
    }

    // ── 4c. appendModelChild 自动分配 ord ─────────────────────────────────
    printf("\n── 子规格追加（ord 自动分配）──\n");
    {
        const std::string db7 = dbDir + "/cfgtest7.db";
        ::unlink(db7.c_str());
        ConfigDb db(db7);
        db.upsertModel({"CLST001","电池簇","cluster"});
        db.upsertModel({"CTL001","控制器","controller"});
        db.upsertModel({"RACK002","电池柜","rack"});

        // RACK002 = 7×CLST001 + 1×CTL001，正是会踩中 ON CONFLICT(model,ord) 的形状
        CHECK(db.appendModelChild("RACK002","CLST001",7), "追加 7×CLST001");
        CHECK(db.appendModelChild("RACK002","CTL001",1),  "追加 1×CTL001");
        auto kids = db.childrenOf("RACK002");
        CHECK(kids.size() == 2, "两项都在（ord 自动递增，未相互覆盖）");
        if (kids.size() == 2) {
            CHECK(kids[0].child_model=="CLST001" && kids[0].count==7, "第一项 7×CLST001");
            CHECK(kids[1].child_model=="CTL001"  && kids[1].count==1, "第二项 1×CTL001");
        }
    }

    // ── 4d. CSV 往返 ──────────────────────────────────────────────────────
    printf("\n── 设备树 CSV 往返 ──\n");
    {
        const std::string db8 = dbDir + "/cfgtest8.db";
        ::unlink(db8.c_str());
        ConfigDb db(db8);

        MigrateStats st; std::string err;
        CHECK(migrateJsonToDb(canonical, db, st, err), "先用真实配置建树");

        std::string csv1;
        CHECK(exportTreeToCsv(db, csv1, err), "导出 CSV");
        int nNodes = 0, nPts = 0;
        for (size_t i = 0; i < csv1.size(); ++i) if (csv1[i]=='\n') ++nNodes;
        printf("    CSV %d 行（含表头）\n", nNodes);
        // 含逗号的描述字段必须被引号包住，否则列会错位
        CHECK(csv1.find(CSV_HEADER_PROBE) != std::string::npos, "表头正确");

        int in_nodes = 0, in_pts = 0;
        CHECK(importTreeFromCsv(csv1, db, in_nodes, in_pts, err),
              "导入同一份 CSV 成功");
        printf("    导入: 节点 %d / 点位 %d\n", in_nodes, in_pts);
        CHECK(in_nodes == st.nodes,  "节点数与原树一致（无点位的节点未丢）");
        CHECK(in_pts   == st.points, "点位数与原树一致");

        std::string csv2;
        CHECK(exportTreeToCsv(db, csv2, err), "再次导出");
        CHECK(csv1 == csv2, "CSV → 库 → CSV 完全一致（列顺序/转义稳定）");
        (void)nPts;
    }

    // ── 4d-2. CSV 往返不得损坏 raw_json ───────────────────────────────────
    // 上面的 csv1==csv2 只证明 10 个列稳定，证明不了 raw_json 完好 —— CSV 列
    // 之外的字段（unit / description / func_code、CAN 的位域）全在 raw_json 里。
    // 判据必须放在 JSON 两端：库 → CSV → 库，导出的 config.json 应逐字段不变。
    printf("\n── CSV 往返对 raw_json 的保真（JSON 两端比对）──\n");
    {
        const std::string dbA = dbDir + "/cfgtest_raw.db";
        ::unlink(dbA.c_str());
        ConfigDb db(dbA);
        MigrateStats st; std::string err;
        migrateJsonToDb(canonical, db, st, err);

        std::string before;
        CHECK(exportDbToJson(db, before, err), "导出基准 JSON");
        std::string csv;
        CHECK(exportTreeToCsv(db, csv, err), "导出 CSV");
        int n = 0, p = 0;
        CHECK(importTreeFromCsv(csv, db, n, p, err), "原样导回");
        std::string after;
        CHECK(exportDbToJson(db, after, err), "再导出 JSON");

        std::vector<std::string> d3;
        diffJson(json::parse(before), json::parse(after), "", d3);
        if (!d3.empty()) {
            printf("    ── 差异 %zu 处 ──\n", d3.size());
            for (size_t i = 0; i < d3.size() && i < 8; ++i)
                printf("      %s\n", d3[i].c_str());
        }
        CHECK(d3.empty(), "CSV 往返后协议专属字段无一丢失（合并而非按列重建）");
    }

    // ── 4d-3. Excel 新增点位：键名与类型必须按协议来 ──────────────────────
    // 4d-2 只覆盖"导出再导回"，两端都有旧 raw_json 可合并。Excel 批量加点走的是
    // 另一条路：全新点位【没有旧值可认】，键名与类型只能按任务协议决定。
    // 曾经一律写 {"address":"<文本>"}，两处都错 —— 键错让 CAN/DLT 点位在协议
    // 解析器眼里凭空消失；类型错让 Modbus 地址成了字符串，整份快照在
    // appConfigFromJsonString 抛 type_error.302，一个新点位废掉整次「生效」。
    printf("\n── Excel 新增点位的协议落键 ──\n");
    {
        const std::string dbN = dbDir + "/cfgtest_newpt.db";
        ::unlink(dbN.c_str());
        ConfigDb db(dbN);
        std::string err;

        // 每种协议一个任务，让新增点位有协议可依
        struct { const char* id; const char* proto; } TASKS[] = {
            {"t_mb","modbus"}, {"t_104","iec104"}, {"t_850","iec61850"},
            {"t_ua","opcua"},  {"t_645","dlt645"}, {"t_698","dlt698"}, {"t_can","can"},
        };
        for (auto& t : TASKS) {
            TaskRow r; r.id = t.id; r.protocol = t.proto; r.endpoint_json = "{}";
            CHECK(db.upsertTask(r), (std::string("建任务 ") + t.id).c_str());
        }

        std::string csv =
            "node_path,node_name,model,point_key,task,addr,dtype,scale,offset,meta_json\n"
            "n_mb,,,temp,t_mb,100,float,0.1,0,{}\n"
            "n_104,,,volt,t_104,1001,13,1,0,{}\n"
            "n_850,,,ain,t_850,IO/GGIO1.AnIn1.mag.f,MX,1,0,{}\n"
            "n_ua,,,spd,t_ua,ns=2;s=Speed,,1,0,{}\n"
            "n_645,,,eng,t_645,00010000,,0.01,0,{}\n"
            "n_698,,,eng,t_698,00200200,,1,0,{}\n"
            "n_can,,,rpm,t_can,0x18FF50E5,,1,0,{}\n";
        int n = 0, p = 0;
        CHECK(importTreeFromCsv(csv, db, n, p, err), "七种协议的新增点位导入成功");
        if (!err.empty()) printf("    错误信息: %s\n", err.c_str());

        // 逐点检查落到了哪个键、什么类型
        struct { const char* path; const char* key; const char* akey; bool num; }
        EXP[] = {
            {"n_mb", "temp","address",   true },
            {"n_104","volt","ioa",       true },
            {"n_850","ain", "object_ref",false},
            {"n_ua", "spd", "node_id",   false},
            {"n_645","eng", "data_id",   false},
            {"n_698","eng", "oad",       false},
            {"n_can","rpm", "can_id",    false},
        };
        std::map<std::string,std::string> raw;   // "路径.键" → raw_json
        for (const auto& nd : db.listNodes())
            for (const auto& pt : db.nodePoints(nd.id))
                raw[nd.path + "." + pt.point_key] = pt.raw_json;

        for (auto& e : EXP) {
            const std::string k = std::string(e.path) + "." + e.key;
            auto it = raw.find(k);
            CHECK(it != raw.end(), (k + " 已入库").c_str());
            if (it == raw.end()) continue;
            json j = json::parse(it->second);
            CHECK(j.contains(e.akey),
                  (k + " 地址落在协议专属键 " + e.akey).c_str());
            if (j.contains(e.akey))
                CHECK(j[e.akey].is_number_integer() == e.num,
                      (k + " 地址类型" + (e.num ? "为整数" : "为字符串")).c_str());
        }
        // IEC104 的 type_id 解析器用 at() 取，缺了会抛 —— 必须从 dtype 列落成整数
        CHECK(json::parse(raw["n_104.volt"]).value("type_id", -1) == 13,
              "IEC104 的 dtype 列落成整数 type_id");
        // DLT 的前导零有意义，绝不能被当数字吃掉
        CHECK(json::parse(raw["n_645.eng"]).value("data_id", std::string()) == "00010000",
              "DLT645 data_id 保留前导零");

        // ── 对照组：旧实现（一律 address + 字符串）──────────────────────
        // 不比对照一遍，容易把"测试通过"错当成"这个 bug 本来就不存在"。
        {
            auto oldImpl = [](const std::string& addr, const std::string& dtype) {
                json rj = json::object();
                if (!addr.empty())  rj["address"]   = addr;   // 永远是字符串
                if (!dtype.empty()) rj["data_type"] = dtype;
                return rj;
            };
            json bad_mb  = oldImpl("100", "float");
            json bad_can = oldImpl("0x18FF50E5", "");
            CHECK(bad_mb["address"].is_string(),
                  "对照组复现：Modbus 地址被写成字符串（快照解析必抛 302）");
            CHECK(!bad_can.contains("can_id") && bad_can.contains("address"),
                  "对照组复现：CAN 信号被写成 address，协议解析器认不出");
        }

        // 全链路判据：新增点位后编译出的快照必须能被运行时配置解析器吃下
        {
            const std::string dbR = dbDir + "/cfgtest_newpt_rt.db";
            ::unlink(dbR.c_str());
            ConfigDb db2(dbR);
            MigrateStats mst; std::string e2;
            CHECK(migrateJsonToDb(canonical, db2, mst, e2), "迁入基准配置");

            std::string c0;
            CHECK(exportTreeToCsv(db2, c0, e2), "导出 CSV");
            // 找一个 modbus 任务，给它加一个新点位（模拟用户在 Excel 里插一行）
            std::string mbTask;
            for (const auto& t : db2.listTasks())
                if (t.protocol == "modbus") { mbTask = t.id; break; }
            CHECK(!mbTask.empty(), "基准配置里有 modbus 任务可供加点");
            if (!mbTask.empty()) {
                c0 += mbTask + ",,,zz_new_point," + mbTask + ",900,uint16,1,0,{}\n";
                int n2 = 0, p2 = 0;
                CHECK(importTreeFromCsv(c0, db2, n2, p2, e2), "导入含新点位的 CSV");
                int64_t ver = 0; CompileStats cst; std::vector<std::string> w;
                CHECK(compileTree(db2, ver, cst, w, e2), "编译");
                std::string rjson; RuntimeStats rst;
                CHECK(buildRuntimeJson(db2, ver, rjson, rst, e2), "构建运行配置");
                bool parsed = true;
                try { (void)appConfigFromJsonString(rjson); }
                catch (const std::exception& ex) {
                    parsed = false;
                    printf("    解析失败: %s\n", ex.what());
                }
                CHECK(parsed, "Excel 加点后的快照能被运行时解析器吃下（原 bug 在此抛 302）");
            }
        }
    }

    // ── 4e. CSV 导入的校验与原子性 ────────────────────────────────────────
    printf("\n── CSV 导入校验 ──\n");
    {
        const std::string db9 = dbDir + "/cfgtest9.db";
        ::unlink(db9.c_str());
        ConfigDb db(db9);
        int n = 0, p = 0; std::string err;

        std::string good =
            "node_path,node_name,model,point_key,task,addr,dtype,scale,offset,meta_json\n"
            "bus,总线,,,,,,,,{}\n"
            "bus.m1,表1,,volt,t1,0201FF00,,0.01,0,{}\n";
        CHECK(importTreeFromCsv(good, db, n, p, err), "合法 CSV 导入成功");
        CHECK(n == 2 && p == 1, "2 节点 1 点位");

        int before = (int)db.countRows("tree_nodes");

        // 父节点缺失 → 建树会断链
        std::string orphan =
            "node_path,node_name,model,point_key,task,addr,dtype,scale,offset,meta_json\n"
            "a.b.c,孤儿,,,,,,,,{}\n";
        n = p = 0;
        CHECK(!importTreeFromCsv(orphan, db, n, p, err), "父节点缺失被拒绝");
        printf("    错误信息: %s\n", err.c_str());

        // 同节点内点位重名 → 会静默覆盖
        std::string dup =
            "node_path,node_name,model,point_key,task,addr,dtype,scale,offset,meta_json\n"
            "x,,,v,t,1,,1,0,{}\n"
            "x,,,v,t,2,,1,0,{}\n";
        n = p = 0;
        CHECK(!importTreeFromCsv(dup, db, n, p, err), "同节点点位重名被拒绝");

        // scale 非数字
        std::string bad =
            "node_path,node_name,model,point_key,task,addr,dtype,scale,offset,meta_json\n"
            "y,,,v,t,1,,abc,0,{}\n";
        n = p = 0;
        CHECK(!importTreeFromCsv(bad, db, n, p, err), "scale 非数字被拒绝");

        CHECK((int)db.countRows("tree_nodes") == before,
              "三次失败的导入都未改动原树（全量校验先于写入）");
    }

    // ── 5. 库结构版本 ─────────────────────────────────────────────────────
    printf("\n── schema 版本 ──\n");
    {
        const std::string db5 = dbDir + "/cfgtest5.db";
        ::unlink(db5.c_str());
        { ConfigDb db(db5); CHECK(db.ok(), "首次建库成功"); }
        { ConfigDb db(db5); CHECK(db.ok(), "重复打开同一库不报错（幂等建表）"); }
    }

    printf("\n════════════════════════════════════\n");
    if (g_fail) { printf("%d 项失败 ✗\n", g_fail); return 1; }
    printf("全部通过 ✓\n");
    return 0;
}
