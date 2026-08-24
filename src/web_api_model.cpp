// src/web_api_model.cpp — 模型驱动板块的 HTTP API
//
// 挂载于既有 WebServer 之上（鉴权由 set_pre_routing_handler 统一处理，本文件
// 无需各自校验）。之所以独立成文件：web_server.cpp 已承载实时监控与旧配置页，
// 六板块的 CRUD 再堆进去会让两套模型的代码交织。
//
// ── 与运行中的采集的关系 ────────────────────────────────────────────────────
// P1b 阶段这些接口只读写 config.db，【不影响正在跑的采集】—— 采集仍按 config.json
// 运行。P2 加入"编译/生效"后，才由编译快照驱动采集。这样设备树可以放心编辑，
// 编到一半不会把现场采集搞挂。

#include "config_db.h"
#include "formula.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace industrial {

namespace {

void sendJson(httplib::Response& res, const json& j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(), "application/json; charset=utf-8");
}

void sendErr(httplib::Response& res, const std::string& msg, int status = 400) {
    sendJson(res, json{{"ok", false}, {"error", msg}}, status);
}

void sendOk(httplib::Response& res, json extra = json::object()) {
    extra["ok"] = true;
    sendJson(res, extra);
}

// 解析请求体；失败时直接回 400 并返回 false
bool parseBody(const httplib::Request& req, httplib::Response& res, json& out) {
    try {
        out = json::parse(req.body);
        return true;
    } catch (const std::exception& e) {
        sendErr(res, std::string("请求体不是合法 JSON: ") + e.what());
        return false;
    }
}

json nodeToJson(const TreeNodeRow& n) {
    json j;
    j["id"]   = n.id;
    j["ord"]  = n.ord;
    j["code"] = n.code;
    j["name"] = n.name;
    j["path"] = n.path;
    if (n.parent_id) j["parent_id"] = *n.parent_id; else j["parent_id"] = nullptr;
    if (n.model)     j["model"]     = *n.model;     else j["model"]     = nullptr;
    try { j["meta"] = json::parse(n.meta_json); } catch (...) { j["meta"] = json::object(); }
    return j;
}

} // namespace

// ── 路由注册 ─────────────────────────────────────────────────────────────────

void registerModelApi(httplib::Server& svr, ConfigDb& db, ReloadFn on_reload) {

    // ══ 设备规格 ══════════════════════════════════════════════════════════════
    svr.Get("/api/models", [&db](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& m : db.listModels()) {
            json j{{"code",m.code},{"name",m.name},{"kind",m.kind}};
            // 列表页要显示"由什么组成"，顺带带上组成与点位数
            json kids = json::array();
            for (const auto& c : db.childrenOf(m.code))
                kids.push_back({{"model",c.child_model},{"count",c.count},{"ord",c.ord}});
            j["children"]    = kids;
            j["point_count"] = db.pointsOf(m.code).size();
            arr.push_back(std::move(j));
        }
        sendJson(res, arr);
    });

    svr.Get(R"(/api/models/([^/]+))", [&db](const httplib::Request& req, httplib::Response& res) {
        const std::string code = req.matches[1];
        json j;
        bool found = false;
        for (const auto& m : db.listModels())
            if (m.code == code) {
                j = {{"code",m.code},{"name",m.name},{"kind",m.kind}};
                found = true; break;
            }
        if (!found) { sendErr(res, "设备规格不存在: " + code, 404); return; }

        json attrs = json::array();
        for (const auto& a : db.attrsOf(code)) attrs.push_back({{"key",a.key},{"value",a.value}});
        json pts = json::array();
        for (const auto& p : db.pointsOf(code))
            pts.push_back({{"key",p.key},{"name",p.name},{"unit",p.unit},{"ord",p.ord}});
        json kids = json::array();
        for (const auto& c : db.childrenOf(code))
            kids.push_back({{"model",c.child_model},{"count",c.count},{"ord",c.ord}});
        j["attrs"] = attrs; j["points"] = pts; j["children"] = kids;
        sendJson(res, j);
    });

    svr.Post("/api/models", [&db](const httplib::Request& req, httplib::Response& res) {
        json b;
        if (!parseBody(req, res, b)) return;
        if (!b.contains("code") || b["code"].get<std::string>().empty())
            { sendErr(res, "缺少 code"); return; }

        ModelRow m{b["code"], b.value("name",""), b.value("kind","")};
        if (!db.upsertModel(m)) { sendErr(res, db.lastError(), 500); return; }

        // 点位与属性为全量替换（前端每次提交完整定义）
        if (b.contains("points")) {
            for (const auto& old : db.pointsOf(m.code)) db.deleteModelPoint(m.code, old.key);
            int ord = 0;
            for (const auto& p : b["points"])
                db.addModelPoint({m.code, p.value("key",""), p.value("name",""),
                                  p.value("unit",""), ord++});
        }
        if (b.contains("attrs"))
            for (const auto& a : b["attrs"])
                db.addModelAttr({m.code, a.value("key",""), a.value("value","")});

        sendOk(res, json{{"code", m.code}});
    });

    // 追加子规格：ord 由后端分配，前端不必也不应关心
    svr.Post(R"(/api/models/([^/]+)/children)",
             [&db](const httplib::Request& req, httplib::Response& res) {
        const std::string code = req.matches[1];
        json b;
        if (!parseBody(req, res, b)) return;
        const std::string child = b.value("model", "");
        const int cnt = b.value("count", 1);
        if (child.empty()) { sendErr(res, "缺少 model"); return; }
        if (cnt < 1)       { sendErr(res, "count 必须 >= 1"); return; }
        if (!db.appendModelChild(code, child, cnt)) {
            // 环检测与外键的拒绝都在这里冒出来，原样回给用户
            sendErr(res, db.lastError());
            return;
        }
        sendOk(res);
    });

    svr.Delete(R"(/api/models/([^/]+)/children/(\d+))",
               [&db](const httplib::Request& req, httplib::Response& res) {
        if (!db.deleteModelChild(req.matches[1], std::stoi(req.matches[2])))
            { sendErr(res, db.lastError(), 500); return; }
        sendOk(res);
    });

    svr.Delete(R"(/api/models/([^/]+))", [&db](const httplib::Request& req, httplib::Response& res) {
        const std::string code = req.matches[1];
        if (db.deleteModel(code)) { sendOk(res); return; }

        // RESTRICT 拒绝时只说"外键约束失败"，用户无从下手。查明是谁在引用：
        // 可能是别的设备规格的组成，也可能是设备树里的实例。
        std::vector<std::string> byModel;
        for (const auto& m : db.listModels())
            for (const auto& c : db.childrenOf(m.code))
                if (c.child_model == code) { byModel.push_back(m.code); break; }
        std::vector<std::string> byNode;
        for (const auto& n : db.listNodes())
            if (n.model && *n.model == code) {
                byNode.push_back(n.path);
                if (byNode.size() >= 5) break;   // 列几个即可，别刷屏
            }

        std::string why = "删除失败：设备规格 " + code + " 仍被引用";
        if (!byModel.empty()) {
            why += "；被设备规格组成引用: ";
            for (size_t i = 0; i < byModel.size(); ++i)
                why += (i ? ", " : "") + byModel[i];
        }
        if (!byNode.empty()) {
            why += "；被设备树节点引用: ";
            for (size_t i = 0; i < byNode.size(); ++i)
                why += (i ? ", " : "") + byNode[i];
            why += " 等";
        }
        sendErr(res, why);
    });

    // ══ 设备树 ══════════════════════════════════════════════════════════════
    //
    // 默认【只返回一层】：带 parent 参数取该节点的直接子节点，不带则取根节点。
    // 前端按需展开 —— 一个储能站 6000+ 节点，整棵树一次下发约 0.9MB 且要渲染
    // 成 6000 行扁平 DOM，页面直接废掉。
    // 需要整棵树的场合（导出、调试）显式传 flat=1。
    svr.Get("/api/tree", [&db](const httplib::Request& req, httplib::Response& res) {
        // 整棵树：仅在明确要求时才给
        if (req.has_param("flat")) {
            json arr = json::array();
            for (const auto& n : db.listNodes()) {
                json j = nodeToJson(n);
                json pts = json::array();
                for (const auto& p : db.nodePoints(n.id))
                    pts.push_back({{"key",p.point_key},{"task",p.task},{"addr",p.addr},
                                   {"dtype",p.dtype},{"scale",p.scale},{"offset",p.offset}});
                j["points"] = pts;
                arr.push_back(std::move(j));
            }
            sendJson(res, arr);
            return;
        }

        std::optional<int64_t> parent;
        if (req.has_param("parent")) {
            try { parent = std::stoll(req.get_param_value("parent")); }
            catch (const std::exception&) { sendErr(res, "parent 不是数字"); return; }
        }
        json arr = json::array();
        for (const auto& kv : db.listChildren(parent)) {
            json j = nodeToJson(kv.first);
            j["child_count"] = kv.second;
            json pts = json::array();
            for (const auto& p : db.nodePoints(kv.first.id))
                pts.push_back({{"key",p.point_key},{"task",p.task},{"addr",p.addr},
                               {"dtype",p.dtype},{"scale",p.scale},{"offset",p.offset}});
            j["points"] = pts;
            arr.push_back(std::move(j));
        }
        sendJson(res, arr);
    });

    svr.Post("/api/tree/node", [&db](const httplib::Request& req, httplib::Response& res) {
        json b;
        if (!parseBody(req, res, b)) return;
        std::optional<int64_t> parent;
        if (b.contains("parent_id") && !b["parent_id"].is_null())
            parent = b["parent_id"].get<int64_t>();
        std::optional<std::string> model;
        if (b.contains("model") && !b["model"].is_null() &&
            !b["model"].get<std::string>().empty())
            model = b["model"].get<std::string>();

        auto id = db.addNode(parent, b.value("code",""), b.value("name",""), model,
                             b.contains("meta") ? b["meta"].dump() : "{}");
        if (!id) { sendErr(res, db.lastError()); return; }
        sendOk(res, json{{"id", *id}});
    });

    svr.Put(R"(/api/tree/node/(\d+))", [&db](const httplib::Request& req, httplib::Response& res) {
        const int64_t id = std::stoll(req.matches[1]);
        json b;
        if (!parseBody(req, res, b)) return;

        // 改名与移动各自重写子树 path，分开处理便于把失败原因说清
        if (b.contains("code") && !db.renameNode(id, b["code"].get<std::string>()))
            { sendErr(res, db.lastError()); return; }
        if (b.contains("parent_id")) {
            std::optional<int64_t> np;
            if (!b["parent_id"].is_null()) np = b["parent_id"].get<int64_t>();
            if (!db.moveNode(id, np)) { sendErr(res, db.lastError()); return; }
        }
        if (b.contains("name") || b.contains("model")) {
            std::optional<std::string> model;
            if (b.contains("model") && !b["model"].is_null() &&
                !b["model"].get<std::string>().empty())
                model = b["model"].get<std::string>();
            if (!db.updateNode(id, b.value("name",""), model))
                { sendErr(res, db.lastError(), 500); return; }
        }
        if (b.contains("meta") && !db.updateNodeMeta(id, b["meta"].dump()))
            { sendErr(res, db.lastError(), 500); return; }
        sendOk(res);
    });

    svr.Delete(R"(/api/tree/node/(\d+))", [&db](const httplib::Request& req, httplib::Response& res) {
        if (!db.deleteNode(std::stoll(req.matches[1])))
            { sendErr(res, db.lastError(), 500); return; }
        sendOk(res);
    });

    // 节点点位：全量替换该节点的点位表
    svr.Put(R"(/api/tree/node/(\d+)/points)",
            [&db](const httplib::Request& req, httplib::Response& res) {
        const int64_t id = std::stoll(req.matches[1]);
        json b;
        if (!parseBody(req, res, b)) return;
        if (!b.is_array()) { sendErr(res, "请求体应为点位数组"); return; }

        if (!db.begin()) { sendErr(res, db.lastError(), 500); return; }
        for (const auto& old : db.nodePoints(id)) db.deleteNodePoint(id, old.point_key);
        int ord = 0;
        for (const auto& p : b) {
            NodePointRow r;
            r.node_id  = id;
            r.point_key= p.value("key","");
            r.task     = p.value("task","");
            r.addr     = p.value("addr","");
            r.dtype    = p.value("dtype","");
            r.scale    = p.value("scale",1.0);
            r.offset   = p.value("offset",0.0);
            r.ord      = ord++;
            r.raw_json = p.dump();
            if (r.point_key.empty()) { db.rollback(); sendErr(res, "点位缺少 key"); return; }
            if (!db.addNodePoint(r)) { db.rollback(); sendErr(res, db.lastError(), 500); return; }
        }
        if (!db.commit()) { db.rollback(); sendErr(res, db.lastError(), 500); return; }
        sendOk(res, json{{"count", b.size()}});
    });

    // 实例化：把设备规格整体展开成设备树子树
    svr.Post("/api/tree/instantiate", [&db](const httplib::Request& req, httplib::Response& res) {
        json b;
        if (!parseBody(req, res, b)) return;
        const std::string model = b.value("model", "");
        const std::string code  = b.value("code", "");
        if (model.empty() || code.empty()) { sendErr(res, "缺少 model 或 code"); return; }

        std::optional<int64_t> parent;
        if (b.contains("parent_id") && !b["parent_id"].is_null())
            parent = b["parent_id"].get<int64_t>();

        // 展开规模上限。默认 20000 够一个储能站（8 台舱约 6000 节点），
        // 又能挡住"数量填成 10000"这类误操作在库里炸出几百万行。
        const int64_t cap = b.value("max_nodes", (int64_t)20000);

        int64_t created = 0;
        std::string err;
        if (!db.instantiateModel(parent, code, b.value("name",""), model,
                                 cap, created, err)) {
            sendErr(res, err);
            return;
        }
        spdlog::info("实例化 {} → {}：创建 {} 个节点", model, code, created);
        sendOk(res, json{{"created", created}});
    });

    // 预估展开规模（前端在确认框里先告诉用户要建多少节点）
    svr.Get(R"(/api/models/([^/]+)/instance_size)",
            [&db](const httplib::Request& req, httplib::Response& res) {
        const int64_t n = db.countInstanceNodes(req.matches[1]);
        if (n < 0) { sendErr(res, "组合过深或规模溢出"); return; }
        sendJson(res, json{{"nodes", n}});
    });

    // ── CSV 导入导出（Excel 批量编辑）──
    svr.Get("/api/tree/csv", [&db](const httplib::Request&, httplib::Response& res) {
        std::string csv, err;
        if (!exportTreeToCsv(db, csv, err)) { sendErr(res, err, 500); return; }
        res.set_header("Content-Disposition", "attachment; filename=\"device_tree.csv\"");
        // BOM：Excel 不认无 BOM 的 UTF-8，中文列名与设备名会显示为乱码
        res.set_content("\xEF\xBB\xBF" + csv, "text/csv; charset=utf-8");
    });

    svr.Post("/api/tree/csv", [&db](const httplib::Request& req, httplib::Response& res) {
        std::string body = req.body;
        // 去掉 Excel 另存为 CSV 时写入的 BOM，否则首列名变成 "\xEF\xBB\xBFnode_path"
        if (body.size() >= 3 && (unsigned char)body[0] == 0xEF &&
            (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF)
            body.erase(0, 3);

        int nodes = 0, points = 0;
        std::string err;
        if (!importTreeFromCsv(body, db, nodes, points, err)) {
            // 校验失败不改动原树，把行号级的原因回给用户
            sendErr(res, err);
            return;
        }
        spdlog::info("设备树 CSV 导入: {} 节点 / {} 点位", nodes, points);
        sendOk(res, json{{"nodes",nodes},{"points",points}});
    });

    // ══ 采集任务 ════════════════════════════════════════════════════════════
    svr.Get("/api/tasks", [&db](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& t : db.listTasks()) {
            json j{{"id",t.id},{"protocol",t.protocol},{"interval_ms",t.interval_ms},
                   {"enabled",t.enabled},{"reconnect_sec",t.reconnect_sec}};
            try { j["endpoint"] = json::parse(t.endpoint_json); }
            catch (...) { j["endpoint"] = json::object(); }
            arr.push_back(std::move(j));
        }
        sendJson(res, arr);
    });

    svr.Post("/api/tasks", [&db](const httplib::Request& req, httplib::Response& res) {
        json b;
        if (!parseBody(req, res, b)) return;
        if (!b.contains("id") || b["id"].get<std::string>().empty())
            { sendErr(res, "缺少 id"); return; }
        TaskRow t;
        t.id            = b["id"];
        t.protocol      = b.value("protocol","modbus");
        t.endpoint_json = b.contains("endpoint") ? b["endpoint"].dump() : "{}";
        t.interval_ms   = b.value("interval_ms",1000);
        t.enabled       = b.value("enabled",true);
        t.reconnect_sec = b.value("reconnect_sec",5);
        if (t.interval_ms < 1) { sendErr(res, "interval_ms 必须 >= 1"); return; }
        if (!db.upsertTask(t)) { sendErr(res, db.lastError(), 500); return; }
        sendOk(res);
    });

    svr.Delete(R"(/api/tasks/([^/]+))", [&db](const httplib::Request& req, httplib::Response& res) {
        if (!db.deleteTask(req.matches[1])) { sendErr(res, db.lastError(), 500); return; }
        sendOk(res);
    });

    // ══ 通讯通道 ════════════════════════════════════════════════════════════
    svr.Get("/api/channels", [&db](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& c : db.listChannels()) {
            json j{{"id",c.id},{"type",c.type},{"direction",c.direction},{"enabled",c.enabled}};
            json cfg;
            try { cfg = json::parse(c.config_json); } catch (...) { cfg = json::object(); }
            // 不回显明文密码（与既有配置页一致，保存时原样送回则视为未修改）
            if (cfg.contains("password") && cfg["password"].is_string() &&
                !cfg["password"].get<std::string>().empty())
                cfg["password"] = "********";
            j["config"] = cfg;
            arr.push_back(std::move(j));
        }
        sendJson(res, arr);
    });

    svr.Post("/api/channels", [&db](const httplib::Request& req, httplib::Response& res) {
        json b;
        if (!parseBody(req, res, b)) return;
        if (!b.contains("id") || b["id"].get<std::string>().empty())
            { sendErr(res, "缺少 id"); return; }
        const std::string id = b["id"];

        json cfg = b.contains("config") ? b["config"] : json::object();
        // 密码为哨兵值 → 沿用库里原值；用户没改就不该被 "********" 覆盖
        if (cfg.contains("password") && cfg["password"] == "********") {
            cfg.erase("password");
            for (const auto& old : db.listChannels())
                if (old.id == id) {
                    try {
                        json oc = json::parse(old.config_json);
                        if (oc.contains("password")) cfg["password"] = oc["password"];
                    } catch (...) {}
                    break;
                }
        }
        ChannelRow c;
        c.id          = id;
        c.type        = b.value("type","mqtt");
        c.direction   = b.value("direction","up");
        c.config_json = cfg.dump();
        c.enabled     = b.value("enabled",true);
        if (!db.upsertChannel(c)) { sendErr(res, db.lastError(), 500); return; }
        sendOk(res);
    });

    svr.Delete(R"(/api/channels/([^/]+))", [&db](const httplib::Request& req, httplib::Response& res) {
        if (!db.deleteChannel(req.matches[1])) { sendErr(res, db.lastError(), 500); return; }
        sendOk(res);
    });

    // ══ 系统设置 ══════════════════════════════════════════════════════════
    // 【不在此处】：Web 端口 / 日志 / 缓存留在 config.json，由既有的
    // GET|POST /api/config/full 承载（那里已实现密码遮罩与端口热更新）。
    // 再开一套 /api/settings 会造成两个写入口争抢同一份配置。

    // ══ 协议绑定（设备规格测点 → 任务 + 地址公式）════════════════════════════
    svr.Get(R"(/api/models/([^/]+)/bindings)",
            [&db](const httplib::Request& req, httplib::Response& res) {
        json arr = json::array();
        for (const auto& b : db.bindingsOf(req.matches[1]))
            arr.push_back({{"point_key",b.point_key},{"task",b.task},
                           {"addr_formula",b.addr_formula},{"dtype",b.dtype},
                           {"scale",b.scale},{"offset",b.offset}});
        sendJson(res, arr);
    });

    svr.Post(R"(/api/models/([^/]+)/bindings)",
             [&db](const httplib::Request& req, httplib::Response& res) {
        const std::string model = req.matches[1];
        json b;
        if (!parseBody(req, res, b)) return;
        const std::string key = b.value("point_key","");
        if (key.empty()) { sendErr(res, "缺少 point_key"); return; }

        // 公式先做语法检查再入库：错误留到「生效」时才发现，用户已经配了几十个
        // 绑定，很难定位是哪一个写错了。此处只查语法（变量要到编译时才知道）。
        const std::string f = b.value("addr_formula","");
        std::string ferr;
        if (!f.empty() && !checkFormulaSyntax(f, ferr))
            { sendErr(res, "地址公式语法错误：\n" + ferr); return; }

        BindingRow r;
        r.model = model; r.point_key = key;
        r.task = b.value("task","");
        r.addr_formula = f;
        r.dtype = b.value("dtype","");
        r.scale = b.value("scale",1.0);
        r.offset = b.value("offset",0.0);
        r.raw_json = b.contains("raw") ? b["raw"].dump() : "{}";
        if (!db.upsertBinding(r)) { sendErr(res, db.lastError(), 500); return; }
        sendOk(res);
    });

    // 公式试算：给定变量值算一遍，供用户在 Web 上边写边看结果
    svr.Post("/api/formula/eval", [](const httplib::Request& req, httplib::Response& res) {
        json b;
        if (!parseBody(req, res, b)) return;
        std::map<std::string,int64_t> vars;
        if (b.contains("vars") && b["vars"].is_object())
            for (auto it = b["vars"].begin(); it != b["vars"].end(); ++it)
                if (it.value().is_number_integer()) vars[it.key()] = it.value();
        std::string err;
        auto v = evalFormula(b.value("formula",""), vars, err);
        if (!v) { sendErr(res, err); return; }
        sendOk(res, json{{"value", *v}});
    });

    // ══ 「生效」：编译设备树 ═════════════════════════════════════════════════
    svr.Post("/api/compile", [&db, on_reload](const httplib::Request&, httplib::Response& res) {
        int64_t ver = 0;
        CompileStats st;
        std::vector<std::string> warns;
        std::string err;
        if (!compileTree(db, ver, st, warns, err)) { sendErr(res, err); return; }

        spdlog::info("编译生效：版本 {} —— 节点 {} / 测点 {}（模型 {} / 覆盖 {}），告警 {}",
                     ver, st.nodes, st.points, st.from_model, st.overridden, st.warnings);

        // 热切换：让正在跑的采集线程换到新点表。走到这里版本已经落库，所以
        // 切换失败不算整体失败 —— 如实回报 applied=false + 原因，用户重启即可。
        bool applied = false;
        std::string reload_msg;
        if (on_reload) {
            applied = on_reload(ver, reload_msg);
            if (!applied) spdlog::warn("{}", reload_msg);
        } else {
            reload_msg = "本进程未接入热切换，重启后生效";
        }

        // 告警只回前 N 条：地址公式写错一处，上万个实例会各报一次，全量下发
        // 既撑爆响应体也淹没信息。总数另给，前端提示"还有 N 条同类问题"。
        constexpr size_t MAX_W = 50;
        json w = json::array();
        for (size_t i = 0; i < warns.size() && i < MAX_W; ++i) w.push_back(warns[i]);
        sendOk(res, json{{"version",ver},{"nodes",st.nodes},{"points",st.points},
                         {"from_model",st.from_model},{"overridden",st.overridden},
                         {"warning_count",st.warnings},
                         {"warnings_truncated", warns.size() > MAX_W},
                         {"warnings",w},
                         {"applied",applied},{"reload_msg",reload_msg}});
    });

    // 查看某版本的编译结果（不带 ver 则取最新；task 可过滤）
    svr.Get("/api/compiled", [&db](const httplib::Request& req, httplib::Response& res) {
        int64_t ver = db.latestVersion();
        if (req.has_param("ver")) {
            try { ver = std::stoll(req.get_param_value("ver")); }
            catch (const std::exception&) { sendErr(res, "ver 不是数字"); return; }
        }
        if (ver <= 0) { sendErr(res, "尚无编译版本，请先执行「生效」", 404); return; }

        const std::string task = req.has_param("task") ? req.get_param_value("task") : "";
        // 万级测点不能一次全下发，默认截断并告知总数
        int limit = 500;
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
        }
        auto rows = db.compiledPoints(ver, task);
        json arr = json::array();
        int n = 0;
        for (const auto& r : rows) {
            if (n++ >= limit) break;
            arr.push_back({{"task",r.task},{"node_path",r.node_path},
                           {"point_key",r.point_key},{"addr",r.addr},
                           {"dtype",r.dtype},{"scale",r.scale},{"offset",r.offset}});
        }
        sendJson(res, json{{"version",ver},{"total",rows.size()},
                           {"returned",arr.size()},{"points",arr}});
    });

    // ══ 整库导入导出（备份 / 穿网闸摆渡）════════════════════════════════════
    svr.Get("/api/config/export", [&db](const httplib::Request&, httplib::Response& res) {
        std::string out, err;
        if (!exportDbToJson(db, out, err)) { sendErr(res, err, 500); return; }
        res.set_header("Content-Disposition", "attachment; filename=\"config_export.json\"");
        res.set_content(out, "application/json; charset=utf-8");
    });

    svr.Post("/api/config/import", [&db](const httplib::Request& req, httplib::Response& res) {
        MigrateStats st;
        std::string err;
        if (!migrateJsonToDb(req.body, db, st, err)) { sendErr(res, err); return; }
        spdlog::info("整库导入: 任务 {} / 节点 {} / 点位 {}", st.tasks, st.nodes, st.points);
        sendOk(res, json{{"tasks",st.tasks},{"nodes",st.nodes},
                         {"points",st.points},{"channels",st.channels}});
    });
}

} // namespace industrial
