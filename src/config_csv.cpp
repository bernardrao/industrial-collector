// src/config_csv.cpp — 设备树 ⇄ CSV
//
// 每个点位一行（而非每个节点一行）：用户在 Excel 里最常做的是"把这一列地址
// 按公式往下拉"，逐点成行才能直接拖拽填充。
//
// 列：node_path, node_name, model, point_key, task, addr, dtype, scale, offset, meta_json
//
// 无点位的节点（如刚建好还没配点的总线）也要占一行，point_key 留空 —— 否则
// 导出再导入会把这些节点整个丢掉。
//
// 导入是【全量替换】而非增量合并：先在内存里全部校验通过，再在一个事务里重建
// 设备树。半成品比彻底失败更糟 —— 用户会以为导入成功，实际丢了一半节点。

#include "config_db.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <map>
#include <sstream>

using json = nlohmann::json;

namespace industrial {

namespace {

// ── RFC4180 ──
// 含逗号/引号/换行的字段必须加引号，内部引号双写。中文单位（如 "kW·h"）不需要
// 转义，但描述里出现逗号很常见，不处理会让整行列数错位。
std::string csvEscape(const std::string& s) {
    bool need = s.find_first_of(",\"\n\r") != std::string::npos;
    if (!need) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else          out += c;
    }
    out += "\"";
    return out;
}

// 解析一行 CSV，返回字段数组。支持引号内的逗号与换行内的双引号。
std::vector<std::string> csvSplit(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inQ = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inQ) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i+1] == '"') { cur += '"'; ++i; }
                else inQ = false;
            } else cur += c;
        } else {
            if (c == '"')      inQ = true;
            else if (c == ',') { out.push_back(cur); cur.clear(); }
            else if (c == '\r'){ /* 吃掉 CRLF 的 CR */ }
            else               cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

const char* CSV_HEADER =
    "node_path,node_name,model,point_key,task,addr,dtype,scale,offset,meta_json";

// 数值写进 CSV 的格式。导入时用同样的格式化把旧值再写一遍与 CSV 文本比对，
// 相同即视为"用户没动过这一格"——否则 float 精度（0.1f 提升为 double 是
// 0.10000000149…）每次往返都会被 CSV 的默认 6 位有效数字截断，用户只想改一个
// 地址，却把满表的系数都悄悄改了。
std::string fmtNum(double v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

// 各协议的地址/类型字段名不同（modbus 用 address+data_type，CAN 用 can_id，
// IEC104 用 ioa+type_id…）。CSV 只有笼统的 addr/dtype 两列，回填时必须写回
// 【原本那个键】，否则一个 CAN 信号会被塞上 address/data_type，从此不再是
// CAN 信号。故从既有 raw_json 里找出它实际用的键，而不是猜一个。
const char* ADDR_KEYS[]  = {"address","ioa","object_ref","node_id","data_id","oad","can_id"};
const char* DTYPE_KEYS[] = {"data_type","type_id","fc"};

const char* findKey(const json& j, const char* const* keys, size_t n) {
    for (size_t i = 0; i < n; ++i) if (j.contains(keys[i])) return keys[i];
    return nullptr;
}

// 把 JSON 里的值按 CSV 单元格的写法呈现，用于"用户是否改过"的比对
std::string cellOf(const json& v) {
    if (v.is_string())         return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_number())         return fmtNum(v.get<double>());
    if (v.is_boolean())        return v.get<bool>() ? "true" : "false";
    return v.dump();
}

// ── 新增点位的 raw_json ──────────────────────────────────────────────────────
// 已有点位靠"从旧 raw_json 里认出它用的键"来回填；【全新点位没有旧值可认】，
// 只能按任务的协议决定键名和类型。早先一律写 {"address": "<文本>"}，两处都错：
//   · 键错 —— CAN 信号被写成 address，DLT 测点被写成 address，协议解析器根本
//     不认，运行时表现为该点凭空消失；
//   · 类型错 —— Modbus/IEC104 的地址是整数，写成字符串会让整份快照在
//     appConfigFromJsonString 里抛 type_error.302，一个新点位废掉整次「生效」。
// 这是 Excel 批量加点的主路径，必须按协议给对。
bool newPointRawJson(const std::string& proto, const std::string& key,
                      const std::string& addr, const std::string& dtype,
                      double scale, double offset, json& out, std::string& err) {
    auto asInt = [&](const std::string& s, int64_t& v) {
        if (s.empty()) return false;
        try {
            size_t used = 0;
            v = std::stoll(s, &used, 0);       // 0 = 自动识别 0x 十六进制
            return used == s.size();
        } catch (const std::exception&) { return false; }
    };

    out = json{{"name",key},{"scale",scale},{"offset",offset}};

    if (proto == "modbus") {
        int64_t a = 0;
        if (!asInt(addr, a)) { err = "Modbus 地址必须是整数，实得 \"" + addr + "\""; return false; }
        out["address"]   = a;
        out["data_type"] = dtype.empty() ? std::string("uint16") : dtype;
    } else if (proto == "iec104") {
        int64_t a = 0, t = 0;
        if (!asInt(addr, a))  { err = "IEC104 的 ioa 必须是整数，实得 \"" + addr + "\""; return false; }
        // type_id 解析器用 at() 取，缺了会抛 —— 在这里挡下来，报得比"快照无法解析"清楚
        if (!asInt(dtype, t)) { err = "IEC104 需要在 dtype 列填写 type_id（整数），实得 \"" + dtype + "\""; return false; }
        out["ioa"]     = a;
        out["type_id"] = t;
    } else if (proto == "iec61850") {
        if (addr.empty()) { err = "IEC61850 需要 object_ref"; return false; }
        out["object_ref"] = addr;
        out["fc"]         = dtype.empty() ? std::string("MX") : dtype;
    } else if (proto == "opcua") {
        if (addr.empty()) { err = "OPC UA 需要 node_id"; return false; }
        out["node_id"] = addr;              // "ns=2;s=Temperature"，始终是字符串
    } else if (proto == "dlt645") {
        if (addr.empty()) { err = "DLT645 需要 data_id"; return false; }
        out["data_id"] = addr;              // "00010000"：前导零有意义，不可转数字
    } else if (proto == "dlt698") {
        if (addr.empty()) { err = "DLT698 需要 oad"; return false; }
        out["oad"] = addr;
    } else if (proto == "can") {
        if (addr.empty()) { err = "CAN 需要 can_id"; return false; }
        out["can_id"] = addr;               // parseCanId 认十六进制字符串
    } else {
        // 任务尚未建立（先导树、后建任务是合理的次序）→ 协议未知，退回通用形状。
        // 这类点位的 task 不在 tasks 表里，编译快照里也就不会有它，所以写得
        // 笼统一点不会污染运行时；等任务建好、用户再导一次 CSV 就会走到上面
        // 的分支去。地址能当整数解析的就存整数，省得后面再猜。
        int64_t a = 0;
        if (!addr.empty()) {
            if (asInt(addr, a)) out["address"] = a;
            else                out["address"] = addr;
        }
        if (!dtype.empty()) out["data_type"] = dtype;
    }
    return true;
}

} // namespace

// ── 导出 ─────────────────────────────────────────────────────────────────────

bool exportTreeToCsv(ConfigDb& db, std::string& csv, std::string& err) {
    if (!db.ok()) { err = "配置库不可用"; return false; }

    std::ostringstream os;
    os << CSV_HEADER << "\n";

    for (const auto& n : db.listNodes()) {
        const auto pts = db.nodePoints(n.id);
        const std::string model = n.model ? *n.model : std::string();

        if (pts.empty()) {
            // 无点位的节点也要留一行，否则往返会丢节点
            os << csvEscape(n.path) << "," << csvEscape(n.name) << ","
               << csvEscape(model)  << ",,,,,,," << csvEscape(n.meta_json) << "\n";
            continue;
        }
        for (const auto& p : pts) {
            os << csvEscape(n.path)      << "," << csvEscape(n.name)  << ","
               << csvEscape(model)       << "," << csvEscape(p.point_key) << ","
               << csvEscape(p.task)      << "," << csvEscape(p.addr)  << ","
               << csvEscape(p.dtype)     << "," << fmtNum(p.scale) << ","
               << fmtNum(p.offset)       << "," << csvEscape(n.meta_json) << "\n";
        }
    }
    csv = os.str();
    return true;
}

// ── 导入 ─────────────────────────────────────────────────────────────────────

bool importTreeFromCsv(const std::string& csv, ConfigDb& db,
                       int& nodes_out, int& points_out, std::string& err) {
    if (!db.ok()) { err = "配置库不可用"; return false; }

    // ── 第一遍：全量解析 + 校验，一个错误都不放过再动库 ──
    struct PendNode {
        std::string path, name, model, meta;
        int         order = 0;                 // 首次出现的行序，保持用户编排
    };
    struct PendPoint {
        std::string path, key, task, addr, dtype;
        double      scale = 1.0, offset = 0.0;
        // 同时留下单元格原文：与旧值的格式化结果比对即可判断用户是否真的改过，
        // 未改过的就保留旧值，避免浮点精度在往返中被逐次截断
        std::string scale_txt, offset_txt;
        int         order = 0;
    };
    std::map<std::string, PendNode> nodeMap;
    std::vector<PendPoint>          points;
    int seq = 0;

    std::istringstream is(csv);
    std::string line;
    int lineNo = 0;
    bool sawHeader = false;

    while (std::getline(is, line)) {
        ++lineNo;
        if (line.empty()) continue;
        auto f = csvSplit(line);

        if (!sawHeader) {
            sawHeader = true;
            // 首行是表头就跳过；不是则按数据行处理（容忍用户删掉表头）
            if (!f.empty() && f[0] == "node_path") continue;
        }
        if (f.size() < 4) {
            err = "第 " + std::to_string(lineNo) + " 行列数不足（至少需 4 列，实得 " +
                  std::to_string(f.size()) + "）";
            return false;
        }
        // 列不足 10 的补空，容忍用户在 Excel 里删了尾部空列
        while (f.size() < 10) f.push_back("");

        const std::string path = f[0];
        if (path.empty()) {
            err = "第 " + std::to_string(lineNo) + " 行 node_path 为空";
            return false;
        }

        auto it = nodeMap.find(path);
        if (it == nodeMap.end())
            nodeMap[path] = PendNode{path, f[1], f[2], f[9].empty() ? "{}" : f[9], seq++};

        if (f[3].empty()) continue;   // 该行只声明节点，无点位

        PendPoint p;
        p.path = path; p.key = f[3]; p.task = f[4];
        p.addr = f[5]; p.dtype = f[6];
        try {
            p.scale  = f[7].empty() ? 1.0 : std::stod(f[7]);
            p.offset = f[8].empty() ? 0.0 : std::stod(f[8]);
        } catch (const std::exception&) {
            err = "第 " + std::to_string(lineNo) + " 行 scale/offset 不是数字: \"" +
                  f[7] + "\" / \"" + f[8] + "\"";
            return false;
        }
        p.scale_txt  = f[7];
        p.offset_txt = f[8];
        p.order = seq++;
        points.push_back(std::move(p));
    }

    if (nodeMap.empty()) { err = "CSV 中没有任何节点"; return false; }

    // 每个非根节点的父路径必须也在表里 —— 否则建树时会断链
    for (const auto& kv : nodeMap) {
        auto dot = kv.first.rfind('.');
        if (dot == std::string::npos) continue;
        const std::string parent = kv.first.substr(0, dot);
        if (nodeMap.find(parent) == nodeMap.end()) {
            err = "节点 \"" + kv.first + "\" 的父节点 \"" + parent + "\" 不在表中";
            return false;
        }
    }

    // 同一节点内点位名不得重复（PRIMARY KEY 会静默覆盖）
    {
        std::map<std::string, std::map<std::string,int>> seen;
        for (const auto& p : points) {
            auto& m = seen[p.path];
            if (++m[p.key] > 1) {
                err = "节点 \"" + p.path + "\" 内点位名重复: \"" + p.key + "\"";
                return false;
            }
        }
    }

    // ── 快照旧点位的 raw_json（在清空之前）────────────────────────────────
    // CSV 只有 10 列，而点位实际字段远不止：CAN 信号有 start_bit / bit_length /
    // byte_order / is_signed，各协议还有 unit / description / func_code。
    // 若按 CSV 列重建 raw_json，这些字段会在一次"Excel 改地址"里全部消失。
    // 故对已存在的点位只覆盖 CSV 真正携带的字段，其余原样保留。
    std::map<std::string, std::map<std::string, std::string>> oldRaw;
    for (const auto& n : db.listNodes())
        for (const auto& p : db.nodePoints(n.id))
            oldRaw[n.path][p.point_key] = p.raw_json;

    // ── 第二遍：一个事务里全量重建 ──
    if (!db.begin()) { err = "开启事务失败: " + db.lastError(); return false; }
    auto fail = [&](const std::string& why) { db.rollback(); err = why; return false; };

    // 清空旧树（node_points 靠 CASCADE 跟着走）
    for (const auto& n : db.listNodes())
        if (!n.parent_id && !db.deleteNode(n.id))
            return fail("清空旧设备树失败: " + db.lastError());

    // 按路径深度排序建树：父节点必须先于子节点存在
    std::vector<const PendNode*> ordered;
    for (const auto& kv : nodeMap) ordered.push_back(&kv.second);
    std::sort(ordered.begin(), ordered.end(), [](const PendNode* a, const PendNode* b) {
        auto depth = [](const std::string& s) {
            int d = 0; for (char c : s) if (c == '.') ++d; return d; };
        int da = depth(a->path), db2 = depth(b->path);
        if (da != db2) return da < db2;
        return a->order < b->order;          // 同深度保持用户在表里的先后
    });

    std::map<std::string, int64_t> idOf;
    for (const auto* n : ordered) {
        auto dot = n->path.rfind('.');
        std::optional<int64_t> parent;
        std::string code = n->path;
        if (dot != std::string::npos) {
            code = n->path.substr(dot + 1);
            auto pit = idOf.find(n->path.substr(0, dot));
            if (pit == idOf.end()) return fail("内部错误：父节点未建立 " + n->path);
            parent = pit->second;
        }
        std::optional<std::string> model;
        if (!n->model.empty()) model = n->model;

        auto id = db.addNode(parent, code, n->name, model, n->meta);
        if (!id) return fail("创建节点 " + n->path + " 失败: " + db.lastError());
        idOf[n->path] = *id;
        ++nodes_out;
    }

    // 新增点位要按协议决定地址字段名/类型（见 newPointRawJson）
    std::map<std::string,std::string> taskProto;
    for (const auto& t : db.listTasks()) taskProto[t.id] = t.protocol;

    for (const auto& p : points) {
        auto it = idOf.find(p.path);
        if (it == idOf.end()) return fail("内部错误：点位找不到节点 " + p.path);
        NodePointRow r;
        r.node_id = it->second; r.point_key = p.key; r.task = p.task;
        r.addr = p.addr; r.dtype = p.dtype;
        r.scale = p.scale; r.offset = p.offset;
        r.ord = p.order;

        // raw_json：已存在的点位【合并】，只改 CSV 携带的字段；新点位才新建。
        json rj;
        bool merged = false;
        auto np = oldRaw.find(p.path);
        if (np != oldRaw.end()) {
            auto pp = np->second.find(p.key);
            if (pp != np->second.end()) {
                try { rj = json::parse(pp->second); merged = rj.is_object(); }
                catch (const std::exception&) { merged = false; }
            }
        }

        if (merged) {
            // 地址/类型写回它原本的键，而不是猜一个 —— 否则 CAN 信号会被塞上
            // address/data_type，从此不再是 CAN 信号
            if (const char* ak = findKey(rj, ADDR_KEYS,
                                         sizeof(ADDR_KEYS)/sizeof(*ADDR_KEYS))) {
                if (p.addr != cellOf(rj[ak])) {          // 用户确实改了地址
                    if (rj[ak].is_number_integer()) {
                        try { rj[ak] = (int64_t)std::stoll(p.addr, nullptr, 0); }
                        catch (const std::exception&) { rj[ak] = p.addr; }
                    } else {
                        rj[ak] = p.addr;
                    }
                }
            } else if (!p.addr.empty()) {
                rj["address"] = p.addr;                  // 原本没有地址字段
            }

            if (const char* dk = findKey(rj, DTYPE_KEYS,
                                         sizeof(DTYPE_KEYS)/sizeof(*DTYPE_KEYS))) {
                if (p.dtype != cellOf(rj[dk])) {
                    if (rj[dk].is_number_integer()) {
                        try { rj[dk] = (int64_t)std::stoll(p.dtype); }
                        catch (const std::exception&) { rj[dk] = p.dtype; }
                    } else {
                        rj[dk] = p.dtype;
                    }
                }
            } else if (!p.dtype.empty()) {
                rj["data_type"] = p.dtype;
            }

            // 数值：单元格文本与旧值的格式化结果一致 = 用户没动过，保留旧值
            if (rj.contains("scale") && p.scale_txt == cellOf(rj["scale"]))
                r.scale = rj["scale"].get<double>();
            else
                rj["scale"] = p.scale;

            if (rj.contains("offset") && p.offset_txt == cellOf(rj["offset"]))
                r.offset = rj["offset"].get<double>();
            else
                rj["offset"] = p.offset;
        } else {
            // 全新点位：CSV 列即全部已知信息，键名与类型按任务协议定
            auto ti = taskProto.find(p.task);
            const std::string proto = ti == taskProto.end() ? std::string() : ti->second;
            std::string perr;
            if (!newPointRawJson(proto, p.key, p.addr, p.dtype,
                                 p.scale, p.offset, rj, perr))
                return fail("点位 " + p.path + "." + p.key + "：" + perr);
        }
        r.raw_json = rj.dump();
        if (!db.addNodePoint(r)) return fail("写入点位 " + p.path + "." + p.key +
                                             " 失败: " + db.lastError());
        ++points_out;
    }

    if (!db.commit()) { db.rollback(); err = "提交失败: " + db.lastError(); return false; }
    return true;
}

} // namespace industrial
