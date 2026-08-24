// src/config_runtime.cpp — 编译快照 → 运行时设备表
//
// 把 tasks + compiled_points 还原成 config.json 里 devices[] 的形状，再交给既有的
// appConfigFromJsonString 解析。这样【七个采集器一行都不用改】—— 它们是现场跑
// 了很久的代码，让它们去理解设备规格只会平添风险。风险集中在这一个转换函数里，
// 而它是可以离线验证的。
//
// ── 测点命名 ────────────────────────────────────────────────────────────────
// 扁平时代一个设备下测点名唯一即可；现在同一个 point_key 会出现在 5760 个电芯上，
// 故运行期的测点名取 "节点路径/测点键"。但对迁移来的老设备（节点路径就等于任务
// id）要保持原样，否则升级后 MQTT 主题全变，下游订阅全断。

#include "config_db.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <map>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace industrial {

namespace {

// 该协议的测点容器键（与 config_migrate.cpp 同一套规则）
const char* containerOf(const std::string& proto) {
    if (proto == "dlt645" || proto == "dlt698") return "meters";
    if (proto == "can")                         return "signals";
    return "points";
}

// 该协议存放数据类型的键名（与 config.cpp 的解析器一一对应）。
// 空串 = 该协议的点位没有类型字段（DLT 靠 BCD 长度、CAN 靠位域描述）。
const char* dtypeKeyOf(const std::string& proto) {
    if (proto == "modbus")   return "data_type";
    if (proto == "iec104")   return "type_id";     // 整数
    if (proto == "iec61850") return "fc";
    return "";
}

// 把编译快照里的权威列写回点位 JSON。
//
// 【为什么必须写】：设备规格驱动的点位，raw_json 是编译器现造的，只带
// name/address/_formula/_node —— scale、offset、data_type 全在 compiled_points
// 的【列】里，不在 raw_json 里。不写回去，解析器就取默认值：scale=1、
// data_type=uint16。症状极其隐蔽 —— 13084 个点全 GOOD，界面上一片绿，但电芯
// 电压显示 3307 而不是 3.307，int16 的负电流会变成 60000 多的正数。
// 迁移来的老点位 raw_json 里本就有这些字段，且与列同源，写回是恒等操作。
void applyCompiledFields(json& p, const ConfigDb::CompiledRow& r,
                          const std::string& proto) {
    p["scale"]  = r.scale;
    p["offset"] = r.offset;
    if (!r.dtype.empty()) {
        const std::string dk = dtypeKeyOf(proto);
        if (!dk.empty()) {
            if (dk == "type_id") {
                try { p[dk] = std::stoll(r.dtype); }
                catch (const std::exception&) { p[dk] = r.dtype; }
            } else {
                p[dk] = r.dtype;
            }
        }
    }
}

// 运行期测点名。node_path 等于任务 id 时说明是迁移来的扁平设备，保持原名。
std::string runtimeName(const std::string& task, const std::string& node_path,
                        const std::string& point_key) {
    if (node_path == task) return point_key;
    // 去掉与任务 id 相同的前缀，"rs485_bus_1.meter_01" → "meter_01"
    std::string rel = node_path;
    if (rel.rfind(task + ".", 0) == 0) rel = rel.substr(task.size() + 1);
    return rel + "/" + point_key;
}

} // namespace

bool buildRuntimeJson(ConfigDb& db, int64_t ver, std::string& json_text,
                      RuntimeStats& stats, std::string& err) {
    if (!db.ok()) { err = "配置库不可用"; return false; }
    if (ver <= 0) { err = "尚无编译版本，请先执行「生效」"; return false; }

    // 节点路径 → meta（DLT 电表的 meter_address 等），供还原 meters[] 用
    std::map<std::string,std::string> nodeMeta;
    for (const auto& n : db.listNodes()) nodeMeta[n.path] = n.meta_json;

    json devices = json::array();

    for (const auto& t : db.listTasks()) {
        if (!t.enabled) { ++stats.disabled_tasks; continue; }

        json blk;
        try { blk = json::parse(t.endpoint_json); }
        catch (const std::exception& e) {
            err = "任务 " + t.id + " 连接参数损坏: " + e.what();
            return false;
        }
        // poll_interval 的权威副本在 tasks.interval_ms（见 config_migrate.cpp）
        blk["poll_interval"] = t.interval_ms / 1000.0;

        const auto rows = db.compiledPoints(ver, t.id);
        if (rows.empty()) {
            // 有任务但快照里没有它的测点：多半是设备树上还没给它挂点
            ++stats.empty_tasks;
            spdlog::warn("任务 {} 在编译版本 {} 中没有任何测点，跳过", t.id, ver);
            continue;
        }

        const std::string container = containerOf(t.protocol);

        if (container == "meters") {
            // DLT：按节点路径分组还原成 meters[]，电表级参数取自节点 meta
            std::map<std::string, json> meters;      // node_path → meter 对象
            std::vector<std::string> order;          // 保持首次出现的顺序
            for (const auto& r : rows) {
                auto it = meters.find(r.node_path);
                if (it == meters.end()) {
                    json m = json::object();
                    // 电表 id 取路径末段
                    auto dot = r.node_path.rfind('.');
                    m["id"] = dot == std::string::npos ? r.node_path
                                                       : r.node_path.substr(dot + 1);
                    auto mi = nodeMeta.find(r.node_path);
                    if (mi != nodeMeta.end()) {
                        try {
                            json meta = json::parse(mi->second);
                            for (auto k = meta.begin(); k != meta.end(); ++k)
                                m[k.key()] = k.value();
                        } catch (const std::exception&) { /* meta 空或坏，忽略 */ }
                    }
                    m["points"] = json::array();
                    meters[r.node_path] = std::move(m);
                    order.push_back(r.node_path);
                }
                json p;
                try { p = json::parse(r.raw_json); }
                catch (const std::exception&) { p = json::object(); }
                p["name"] = r.point_key;              // 表内测点名保持原样
                applyCompiledFields(p, r, t.protocol);
                meters[r.node_path]["points"].push_back(std::move(p));
                ++stats.points;
            }
            json arr = json::array();
            for (const auto& path : order) arr.push_back(meters[path]);
            blk["meters"] = arr;
            stats.meters += (int64_t)order.size();
        } else {
            json arr = json::array();
            for (const auto& r : rows) {
                json p;
                try { p = json::parse(r.raw_json); }
                catch (const std::exception&) { p = json::object(); }
                p["name"] = runtimeName(t.id, r.node_path, r.point_key);
                applyCompiledFields(p, r, t.protocol);
                // 编译期算出的地址是权威值：raw_json 里可能残留公式展开前的旧地址
                if (!r.addr.empty()) {
                    // 地址字段名随协议而异，沿用 raw_json 里已有的那个键
                    static const char* AK[] = {"address","ioa","object_ref",
                                               "node_id","data_id","oad","can_id"};
                    const char* used = nullptr;
                    for (auto k : AK) if (p.contains(k)) { used = k; break; }
                    if (used) {
                        if (p[used].is_number_integer()) {
                            try { p[used] = std::stoll(r.addr); }
                            catch (const std::exception&) { p[used] = r.addr; }
                        } else {
                            p[used] = r.addr;
                        }
                    }
                }
                arr.push_back(std::move(p));
                ++stats.points;
            }
            blk[container] = arr;
        }

        json d = json::object();
        d["id"]       = t.id;
        d["enabled"]  = true;
        d["protocol"] = t.protocol;
        d["reconnect_interval_sec"] = t.reconnect_sec;
        d[t.protocol] = blk;
        devices.push_back(std::move(d));
        ++stats.tasks;
    }

    if (devices.empty()) {
        err = "编译版本 " + std::to_string(ver) + " 里没有任何可运行的任务";
        return false;
    }

    json out = json::object();
    out["devices"] = devices;
    json_text = out.dump();
    return true;
}

} // namespace industrial
