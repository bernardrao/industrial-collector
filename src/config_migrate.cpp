// src/config_migrate.cpp — config.json ⇄ config.db
//
// 迁移在 JSON 层面工作，不做逐字段结构体映射：协议块拆成"连接参数"与"点位容器"
// 两部分，点位整条原样存 raw_json。新增协议字段无需改动本文件，也就不存在
// "忘了搬某个字段"这种静默丢失 —— 无损是构造性的，不是靠人工核对清单。
//
// 输入应为 appConfigToJsonString() 的规范形式（而非用户手写的原始文件）：
// 协议简写已归一（canfd → can + fd:true）、默认值已补全，故本文件无需再处理简写。

#include "config_db.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <set>

using json = nlohmann::json;

namespace industrial {

namespace {

// 协议块里承载点位的数组键。这些从 endpoint_json 中剥离，单独进设备树；
// 其余键（host/port/poll_interval…）留在 endpoint_json。
// 注意 modbus.ranges 不在此列：它是"点位生成器"而非点位本身，整体留在连接参数里，
// 由 P2 编译器展开。
const char* POINT_CONTAINERS[] = {"points", "meters", "signals"};

bool isPointContainer(const std::string& k) {
    for (auto c : POINT_CONTAINERS) if (k == c) return true;
    return false;
}

// 某协议的点位容器键。必须由协议决定，不能靠"有没有子节点/有没有点位"反推：
// 一条 meters 为空的 DLT 总线（用户刚建好总线、还没添表）会被反推成"无子节点"，
// 于是 meters 键丢失、凭空多出一个 points 键。P1b 的 Web UI 恰好会造出这种状态。
const char* containerKeyOf(const std::string& proto) {
    if (proto == "dlt645" || proto == "dlt698") return "meters";
    if (proto == "can")                         return "signals";
    return "points";
}

// 点位在节点内的唯一键。各协议点位都有 name 字段。
// 同一节点内 name 重复会因 PRIMARY KEY(node_id,point_key) 相互覆盖而静默丢点，
// 故调用方必须先查重。
std::string pointKeyOf(const json& p) {
    if (p.contains("name") && p["name"].is_string()) return p["name"].get<std::string>();
    return {};
}

double numOr(const json& j, const char* key, double def) {
    if (j.contains(key) && j[key].is_number()) return j[key].get<double>();
    return def;
}

// 各协议的"地址"字段名不同，抽出来放进可查询列（raw_json 里仍有完整原文）
std::string addrOf(const json& p) {
    static const char* KEYS[] = {"address","ioa","object_ref","node_id","data_id","oad","can_id"};
    for (auto k : KEYS) {
        if (!p.contains(k)) continue;
        const auto& v = p[k];
        if (v.is_string())        return v.get<std::string>();
        if (v.is_number_integer())return std::to_string(v.get<int64_t>());
        if (v.is_number())        return std::to_string(v.get<double>());
    }
    return {};
}

std::string dtypeOf(const json& p) {
    for (auto k : {"data_type","type_id","fc"}) {
        if (p.contains(k)) {
            const auto& v = p[k];
            if (v.is_string())         return v.get<std::string>();
            if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
        }
    }
    return {};
}

} // namespace

// ── config.json → config.db ──────────────────────────────────────────────────

bool migrateJsonToDb(const std::string& json_text, ConfigDb& db,
                     MigrateStats& stats, std::string& err) {
    if (!db.ok()) { err = "配置库不可用"; return false; }

    json j;
    try {
        j = json::parse(json_text);
    } catch (const std::exception& e) {
        err = std::string("配置 JSON 解析失败: ") + e.what();
        return false;
    }

    if (!db.begin()) { err = "开启事务失败: " + db.lastError(); return false; }

    // 任何一步失败都整体回滚 —— 迁移留下半成品比彻底失败更糟：
    // 用户会以为迁移成功，实际丢了一半设备。
    auto fail = [&](const std::string& why) {
        db.rollback();
        err = why;
        return false;
    };

    // ── 设备 → 采集任务 + 设备树节点 ──
    if (j.contains("devices")) {
        for (const auto& d : j["devices"]) {
            if (!d.contains("id") || !d.contains("protocol"))
                return fail("设备条目缺少 id 或 protocol");

            const std::string id    = d["id"].get<std::string>();
            const std::string proto = d["protocol"].get<std::string>();

            if (!d.contains(proto) || !d[proto].is_object())
                return fail("设备 " + id + " 缺少协议块 \"" + proto + "\"");
            const json& blk = d[proto];

            // 连接参数 = 协议块剔除点位容器，且剔除 poll_interval。
            // poll_interval 已提升为 tasks.interval_ms 这一权威副本；若两处都留，
            // P1b 的"改采集周期"会写 interval_ms 而导出读 endpoint_json，改动静默丢失。
            // enabled / reconnect_sec 同理，本就只存在于 tasks。
            json endpoint = json::object();
            for (auto it = blk.begin(); it != blk.end(); ++it)
                if (!isPointContainer(it.key()) && it.key() != "poll_interval")
                    endpoint[it.key()] = it.value();

            TaskRow t;
            t.id            = id;
            t.protocol      = proto;
            t.endpoint_json = endpoint.dump();
            t.interval_ms   = (int)(numOr(blk, "poll_interval", 1.0) * 1000.0);
            t.enabled       = d.value("enabled", true);
            t.reconnect_sec = (int)numOr(d, "reconnect_interval_sec", 5);
            if (!db.upsertTask(t)) return fail("写入任务 " + id + " 失败: " + db.lastError());
            ++stats.tasks;

            // 设备本身 = 一个自由节点（无设备规格，采集参数手工配置）
            auto devNode = db.addNode(std::nullopt, id, "", std::nullopt);
            if (!devNode) return fail("创建设备节点 " + id + " 失败: " + db.lastError());
            ++stats.nodes;

            // 把一组点位挂到某节点上
            auto attachPoints = [&](int64_t node, const json& arr,
                                    const std::string& whose) -> bool {
                std::set<std::string> seen;
                int ord = 0;
                for (const auto& p : arr) {
                    const std::string key = pointKeyOf(p);
                    if (key.empty())
                        { err = whose + " 有点位缺少 name 字段"; return false; }
                    if (!seen.insert(key).second)
                        { err = whose + " 内点位名重复: \"" + key +
                                "\"（同节点内必须唯一，否则相互覆盖）"; return false; }

                    NodePointRow np;
                    np.node_id   = node;
                    np.point_key = key;
                    np.task      = id;
                    np.addr      = addrOf(p);
                    np.dtype     = dtypeOf(p);
                    np.scale     = numOr(p, "scale", 1.0);
                    np.offset    = numOr(p, "offset", 0.0);
                    np.raw_json  = p.dump();
                    np.ord       = ord++;
                    if (!db.addNodePoint(np))
                        { err = "写入点位失败: " + db.lastError(); return false; }
                    ++stats.points;
                }
                return true;
            };

            // 容器由协议决定（不是"块里有哪个键就用哪个"，那样两个键并存时会双挂）
            const std::string container = containerKeyOf(proto);

            // DLT645/698 是两级结构：总线(任务) → 电表(子节点) → 点位。
            // 正好印证"任务 + 设备树"的两级设计。
            if (container == "meters" && blk.contains("meters")) {
                for (const auto& m : blk["meters"]) {
                    if (!m.contains("id")) return fail("设备 " + id + " 的电表缺少 id");
                    const std::string mid = m["id"].get<std::string>();

                    // 电表级元数据（meter_address 等）非点位，存节点 meta
                    json meta = json::object();
                    for (auto it = m.begin(); it != m.end(); ++it)
                        if (it.key() != "id" && !isPointContainer(it.key()))
                            meta[it.key()] = it.value();

                    auto mNode = db.addNode(*devNode, mid, "", std::nullopt, meta.dump());
                    if (!mNode)
                        return fail("创建电表节点 " + id + "." + mid + " 失败: " + db.lastError());
                    ++stats.nodes;

                    if (m.contains("points") &&
                        !attachPoints(*mNode, m["points"], "电表 " + id + "." + mid))
                        return fail(err);
                }
            }
            // 其余协议：点位直接挂设备节点
            else if (container != "meters" && blk.contains(container)) {
                if (!attachPoints(*devNode, blk[container], "设备 " + id))
                    return fail(err);
            }
        }
    }

    // ── MQTT 【不入库】────────────────────────────────────────────────────
    //
    // 现有 MQTT 仍由 config.json 承载、由运行中的采集直接读取。若同时写进
    // channels 表，就有了两个写入口：系统设置页改 config.json，通道页改
    // config.db，用户在通道页改完 broker 地址、看到"保存成功"，实际毫无效果。
    // 单一权威副本的道理与 poll_interval 相同。
    //
    // P3 引入真正的多通道（Kafka / 文件 / 下行控制）时，再把 MQTT 一并迁进
    // channels 表并同时撤掉系统设置页的 MQTT 页签，避免中间态出现双写。

    // ── web / logging / cache 【不入库】，留在 config.json ──
    //
    // 职责划分：系统设置（Web 端口、日志、缓存）少量、需要人工可编辑 —— 一旦
    // 鉴权配错或端口冲突进不去 Web，得能用文本编辑器直接改回来；塞进 SQLite
    // 就只能靠程序自己救自己。
    // 而采集侧的设备规格、设备树、测点、任务是成千上万行的批量数据，正是 SQLite
    // 该管的部分。故 config.json 保留系统设置，config.db 承载采集配置。

    if (!db.commit()) { db.rollback(); err = "提交失败: " + db.lastError(); return false; }
    return true;
}

// ── config.db → config.json ──────────────────────────────────────────────────
// 与 migrateJsonToDb 互为逆运算。二者的一致性由 config_db_test 的语义往返
// 测试守护：原始 JSON → 迁移 → 导出，两份 JSON 必须逐字段相等。

bool exportDbToJson(ConfigDb& db, std::string& json_text, std::string& err) {
    if (!db.ok()) { err = "配置库不可用"; return false; }

    json out = json::object();

    // mqtt 不由本库承载（见 migrateJsonToDb 中的说明），故不导出。
    // web / logging / cache 不由本库承载（留在 config.json），故不导出。
    // 导出结果 = 采集配置（devices + 通道），与 config.json 的系统设置段互补。

    // ── 任务 + 设备树 → devices ──
    const auto nodes = db.listNodes();

    // 设备节点（根级）与其子节点（电表）
    auto childrenOfNode = [&](int64_t parent) {
        std::vector<TreeNodeRow> kids;
        for (const auto& n : nodes)
            if (n.parent_id && *n.parent_id == parent) kids.push_back(n);
        return kids;
    };

    // 把节点上的点位还原成原始 JSON 数组（raw_json 即原文，无损）
    auto pointsJson = [&](int64_t node, json& arr) -> bool {
        for (const auto& p : db.nodePoints(node)) {
            try { arr.push_back(json::parse(p.raw_json)); }
            catch (const std::exception& e) {
                err = std::string("点位 raw_json 损坏: ") + e.what();
                return false;
            }
        }
        return true;
    };

    json devArr = json::array();
    for (const auto& t : db.listTasks()) {
        // 任务 id 与设备根节点的 code 一一对应
        const TreeNodeRow* devNode = nullptr;
        for (const auto& n : nodes)
            if (!n.parent_id && n.code == t.id) { devNode = &n; break; }
        if (!devNode) { err = "任务 " + t.id + " 找不到对应设备节点"; return false; }

        json d = json::object();
        d["id"]       = t.id;
        d["enabled"]  = t.enabled;
        d["protocol"] = t.protocol;
        d["reconnect_interval_sec"] = t.reconnect_sec;

        json blk;
        try { blk = json::parse(t.endpoint_json); }
        catch (const std::exception& e) {
            err = std::string("任务 ") + t.id + " 连接参数损坏: " + e.what();
            return false;
        }

        // 采集周期从权威副本 interval_ms 还原
        blk["poll_interval"] = t.interval_ms / 1000.0;

        // 容器键由协议决定并【总是】写出：空 meters 的总线也必须导出 "meters": []，
        // 否则该键丢失、还凭空多出 points。靠子节点数量反推会在此处失手。
        const std::string container =
            (t.protocol == "dlt645" || t.protocol == "dlt698") ? "meters"
          : (t.protocol == "can")                              ? "signals"
                                                               : "points";
        if (container == "meters") {
            // DLT 总线：子节点还原为 meters[]
            json meters = json::array();
            for (const auto& k : childrenOfNode(devNode->id)) {
                json m = json::object();
                m["id"] = k.code;
                try {
                    json meta = json::parse(k.meta_json);
                    for (auto it = meta.begin(); it != meta.end(); ++it)
                        m[it.key()] = it.value();
                } catch (const std::exception&) { /* meta 为空或损坏则忽略 */ }
                json pts = json::array();
                if (!pointsJson(k.id, pts)) return false;
                m["points"] = pts;
                meters.push_back(std::move(m));
            }
            blk["meters"] = meters;
        } else {
            // 其余协议：点位挂在设备节点，还原到 points[] 或 signals[]
            json pts = json::array();
            if (!pointsJson(devNode->id, pts)) return false;
            blk[container] = pts;
        }

        d[t.protocol] = blk;
        devArr.push_back(std::move(d));
    }
    out["devices"] = devArr;

    json_text = out.dump(2);
    return true;
}

} // namespace industrial
