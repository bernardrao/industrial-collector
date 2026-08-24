// src/config_compile.cpp — 「生效」：设备树 + 设备规格 → 编译点表
//
// 编译把"模板 + 位置"变成"具体地址"：一个 CELL001 模型上写着
// `1000 + idx_PACK*128 + idx_CELL*2`，5760 个电芯实例各自算出自己的寄存器地址。
//
// ── 为什么要有编译这一步 ────────────────────────────────────────────────────
// 运行期只读编译快照，不读设备树。于是：
//   · 编辑设备树、改公式都不影响正在跑的采集 —— 编到一半不会把现场搞挂；
//   · 公式求值集中在一次事务里，采集线程的热路径上没有任何解析开销；
//   · 快照带版本号，出问题可回滚到上一版。
//
// ── 失败即整体回滚 ──────────────────────────────────────────────────────────
// 任何一个公式求值失败都终止整次编译。"一半地址正确、一半悬空"的快照比编译
// 失败危险得多 —— 后者你立刻知道，前者要等到现场读出一堆莫名其妙的数才发现。

#include "config_db.h"
#include "formula.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace industrial {

namespace {

// 节点在【同模型兄弟】中的序号（0 基）。
//
// 用"同模型兄弟"而非"全部兄弟"计数，是因为异构组合的存在：RACK002 由
// 7×CLST001 + 1×CTL001 组成，控制器混在簇中间。若按全部兄弟排序，控制器会把
// 它之后的簇序号整体顶偏一位，公式算出的地址随之全错。按模型分别计数，
// 7 个簇就是 idx_CLST001 = 0..6，控制器是 idx_CTL001 = 0，互不干扰。
std::map<int64_t,int> computeSiblingIndex(const std::vector<TreeNodeRow>& nodes) {
    std::map<int64_t,int> idx;
    // (父id, 模型) → 已见计数
    std::map<std::pair<int64_t,std::string>,int> counter;
    for (const auto& n : nodes) {
        const int64_t parent = n.parent_id ? *n.parent_id : -1;
        const std::string m = n.model ? *n.model : std::string();
        idx[n.id] = counter[{parent, m}]++;
    }
    return idx;
}

} // namespace

bool compileTree(ConfigDb& db, int64_t& ver, CompileStats& stats,
                 std::vector<std::string>& warnings, std::string& err) {
    if (!db.ok()) { err = "配置库不可用"; return false; }

    // listNodes 按 (parent_id, ord) 返回，父节点必先于子节点出现，
    // 故一趟遍历即可自上而下累积祖先变量。
    const auto nodes = db.listNodes();
    if (nodes.empty()) { err = "设备树为空，无可编译内容"; return false; }

    const auto sibIdx = computeSiblingIndex(nodes);

    // 每个型号已实例化的个数 —— 用于 gidx_<MODEL>（全树同型号序号）
    std::map<std::string,int64_t> globalIdx;
    // 节点 id → 该节点处可见的公式变量（含自身与全部祖先）
    std::map<int64_t, std::map<std::string,int64_t>> varsOf;
    // 模型定义缓存：编译 5760 个电芯时不必查 5760 次库
    std::map<std::string, std::vector<BindingRow>> bindCache;
    std::map<std::string, size_t> pointCountCache;

    auto bindingsFor = [&](const std::string& m) -> const std::vector<BindingRow>& {
        auto it = bindCache.find(m);
        if (it == bindCache.end()) it = bindCache.emplace(m, db.bindingsOf(m)).first;
        return it->second;
    };

    if (!db.begin()) { err = "开启事务失败: " + db.lastError(); return false; }
    auto fail = [&](const std::string& why) { db.rollback(); err = why; return false; };

    auto verOpt = db.newVersion("compileTree");
    if (!verOpt) return fail("创建编译版本失败: " + db.lastError());
    ver = *verOpt;

    // 已用过的 (task, addr) —— 两个测点抢同一个地址通常是公式写错，
    // 不致命但必须报出来，否则现场会看到两个点读出同一个值而百思不解。
    std::map<std::string, std::map<std::string,std::string>> addrOwner;

    // ── 地址唯一性的【作用域随协议而变】────────────────────────────────────
    // 一刀切地按"同任务内地址不得重复"检查会产生大量误报，把真问题淹掉：
    //
    //   CAN            同一帧按 bit 偏移打包多个信号是常态，共享 CAN ID 恰恰
    //                  是正确配置 → 完全不检查
    //   DLT645/698     一根总线挂多块表，每块表有独立的数据标识空间，
    //                  两块表都有 data_id=00010000 完全正常 → 按"表"分别检查
    //   其余协议       一个连接一个地址空间 → 按任务检查
    //
    // 实测：不区分时一个储能站刷出近万条告警，其中绝大多数是 CAN 与 DLT 误报。
    std::map<std::string,std::string> taskProto;
    for (const auto& t : db.listTasks()) taskProto[t.id] = t.protocol;

    auto checkDup = [&](const std::string& task, const std::string& addr,
                        const std::string& node_path, const std::string& who,
                        const std::string& hint) {
        if (addr.empty()) return;
        auto pit = taskProto.find(task);
        const std::string proto = pit == taskProto.end() ? std::string() : pit->second;
        if (proto == "can") return;

        // DLT 的地址空间以"表"为界，故把表节点并入作用域键
        const std::string scope =
            (proto == "dlt645" || proto == "dlt698") ? task + "\x1f" + node_path : task;

        auto& owner = addrOwner[scope];
        auto it = owner.find(addr);
        if (it != owner.end()) {
            warnings.push_back("任务 " + task + " 地址 " + addr + " 重复：" +
                               it->second + " 与 " + who + hint);
            ++stats.warnings;
        } else {
            owner[addr] = who;
        }
    };

    for (const auto& n : nodes) {
        // ── 组装本节点可见的变量 ──
        std::map<std::string,int64_t> vars;
        if (n.parent_id) {
            auto pit = varsOf.find(*n.parent_id);
            if (pit != varsOf.end()) vars = pit->second;   // 继承祖先的
        }
        const int myIdx = sibIdx.at(n.id);
        vars["idx"] = myIdx;                                // 本级序号的别名
        if (n.model) {
            vars["idx_" + *n.model] = myIdx;                // 按模型命名，供子孙引用

            // 全局序号：该实例在【整棵树同型号实例】中的序号（0 基，树序）。
            //
            // 为什么需要它：结构化地址（舱*N + 簇*M + …）要求公式能引用每一级
            // 祖先，但异构树里做不到 —— 电芯挂在 RACK001 下时只有 idx_RACK001，
            // 挂在 RACK002 下时只有 idx_RACK002，一条公式引用不了两者。
            // 用 gidx_CELL001*2 + 基址 就能给全站 5760 个电芯排出连续且唯一的
            // 地址，不必关心它在哪种机舱下。
            vars["gidx_" + *n.model] = globalIdx[*n.model]++;
            vars["gidx"] = vars["gidx_" + *n.model];
        }
        varsOf[n.id] = vars;

        // ── 节点自带的测点（自由节点，或模型实例上的手工覆盖）──
        // 地址已是字面量，无需求值；同 key 时覆盖优先于模型。
        std::set<std::string> overridden;
        for (const auto& p : db.nodePoints(n.id)) {
            if (p.task.empty()) {
                warnings.push_back("节点 " + n.path + " 的测点 " + p.point_key +
                                   " 未绑定采集任务，已跳过");
                ++stats.warnings;
                continue;
            }
            checkDup(p.task, p.addr, n.path, n.path + "." + p.point_key, "");

            if (!db.addCompiledPoint(ver, p.task, n.path, p.point_key, p.addr,
                                     p.dtype, p.scale, p.offset, p.raw_json))
                return fail("写入编译点失败: " + db.lastError());
            overridden.insert(p.point_key);
            ++stats.points;
            ++stats.overridden;
        }

        if (!n.model) {
            if (!overridden.empty()) ++stats.nodes;
            continue;
        }

        // ── 设备规格绑定：按公式展开 ──
        const auto& binds = bindingsFor(*n.model);
        if (binds.empty()) {
            // 模型有测点定义却没有任何绑定 = 采不到数据，属于配置没写完
            auto pc = pointCountCache.find(*n.model);
            if (pc == pointCountCache.end())
                pc = pointCountCache.emplace(*n.model, db.pointsOf(*n.model).size()).first;
            if (pc->second > 0 && overridden.empty()) {
                // 同一个模型只报一次，5760 个电芯会刷屏
                const std::string w = "设备规格 " + *n.model + " 定义了 " +
                    std::to_string(pc->second) + " 个测点但没有任何协议绑定，其实例采不到数据";
                if (std::find(warnings.begin(), warnings.end(), w) == warnings.end()) {
                    warnings.push_back(w);
                    ++stats.warnings;
                }
            }
            if (!overridden.empty()) ++stats.nodes;
            continue;
        }

        bool anyPoint = false;
        for (const auto& b : binds) {
            if (overridden.count(b.point_key)) continue;   // 节点覆盖优先
            if (b.task.empty()) {
                warnings.push_back("设备规格 " + *n.model + " 的测点 " + b.point_key +
                                   " 未指定采集任务，已跳过");
                ++stats.warnings;
                continue;
            }

            std::string ferr;
            auto addr = evalFormula(b.addr_formula, vars, ferr);
            if (!addr)
                return fail("节点 " + n.path + " 的测点 " + b.point_key +
                            " 地址公式求值失败：\n" + ferr);

            const std::string addrStr = std::to_string(*addr);
            checkDup(b.task, addrStr, n.path, n.path + "." + b.point_key,
                     "（公式 " + b.addr_formula + " 可能写错）");

            // raw_json 带上求值结果与来源，便于在 Web 上回溯"这个地址怎么来的"
            json rj;
            try { rj = b.raw_json.empty() ? json::object() : json::parse(b.raw_json); }
            catch (const std::exception&) { rj = json::object(); }
            rj["name"]     = b.point_key;
            rj["address"]  = *addr;
            rj["_formula"] = b.addr_formula;
            rj["_node"]    = n.path;

            if (!db.addCompiledPoint(ver, b.task, n.path, b.point_key, addrStr,
                                     b.dtype, b.scale, b.offset, rj.dump()))
                return fail("写入编译点失败: " + db.lastError());
            ++stats.points;
            ++stats.from_model;
            anyPoint = true;
        }
        if (anyPoint || !overridden.empty()) ++stats.nodes;
    }

    if (stats.points == 0)
        return fail("编译结果为空：设备树里没有任何可采集的测点"
                    "（自由节点需配测点，模型实例需配协议绑定）");

    if (!db.commit()) { db.rollback(); err = "提交失败: " + db.lastError(); return false; }

    // 只保留最近若干版本 —— 万级点位场景下每版本上万行
    db.pruneVersions(10);
    return true;
}

} // namespace industrial
