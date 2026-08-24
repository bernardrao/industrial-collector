// src/config_db.cpp — 配置库存储层实现
#include "config_db.h"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <cctype>
#include <ctime>
#include <cstring>
#include <functional>
#include <map>

namespace industrial {

// ── RAII 语句句柄：任何提前 return 都不会漏 finalize ─────────────────────────
namespace {
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK) st_ = nullptr;
    }
    ~Stmt() { if (st_) sqlite3_finalize(st_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    explicit operator bool() const { return st_ != nullptr; }
    sqlite3_stmt* get() const { return st_; }

    void bindText(int i, const std::string& v) {
        sqlite3_bind_text(st_, i, v.c_str(), -1, SQLITE_TRANSIENT);
    }
    void bindInt (int i, int64_t v)  { sqlite3_bind_int64(st_, i, v); }
    void bindReal(int i, double v)   { sqlite3_bind_double(st_, i, v); }
    void bindNull(int i)             { sqlite3_bind_null(st_, i); }

    bool step()      { return sqlite3_step(st_) == SQLITE_ROW; }
    bool done()      { return sqlite3_step(st_) == SQLITE_DONE; }

    std::string text(int c) {
        auto p = reinterpret_cast<const char*>(sqlite3_column_text(st_, c));
        return p ? std::string(p) : std::string();
    }
    bool    isNull(int c) { return sqlite3_column_type(st_, c) == SQLITE_NULL; }
    int64_t i64(int c)    { return sqlite3_column_int64(st_, c); }
    double  real(int c)   { return sqlite3_column_double(st_, c); }

private:
    sqlite3*      db_ = nullptr;
    sqlite3_stmt* st_ = nullptr;
};
} // namespace

// ── 生命周期 ─────────────────────────────────────────────────────────────────

ConfigDb::ConfigDb(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        last_error_ = db_ ? sqlite3_errmsg(db_) : "sqlite3_open 失败";
        spdlog::error("配置库打开失败 {}: {}", db_path, last_error_);
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return;
    }
    // 外键约束默认【关闭】，不开这一句所有 REFERENCES 声明都形同虚设：
    // 删除设备规格不会被已实例化的节点挡住，删除节点会留下孤儿覆盖点。
    exec("PRAGMA foreign_keys=ON;");
    // 配置库低频写、重一致性，不用 WAL（那是 cache.db 的高频写场景）。
    exec("PRAGMA synchronous=FULL;");

    if (!createSchema() || !migrateSchema()) {
        spdlog::error("配置库结构初始化失败: {}", last_error_);
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    spdlog::info("配置库就绪: {} (schema v{})", db_path, CONFIG_DB_SCHEMA_VERSION);
}

ConfigDb::~ConfigDb() {
    if (db_) sqlite3_close(db_);
}

bool ConfigDb::exec(const char* sql) {
    std::lock_guard<std::mutex> lk(mtx_);
    return exec_locked(sql);
}

bool ConfigDb::exec_locked(const char* sql) {
    if (!db_) return false;
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        last_error_ = err ? err : "?";
        spdlog::warn("配置库 SQL 失败: {} ({})", last_error_, sql);
        sqlite3_free(err);
        return false;
    }
    return true;
}

// ── 表结构 ───────────────────────────────────────────────────────────────────

bool ConfigDb::createSchema() {
    static const char* DDL =
        "CREATE TABLE IF NOT EXISTS schema_version(ver INTEGER PRIMARY KEY);"

        // ── 设备规格 ──
        "CREATE TABLE IF NOT EXISTS models("
        "  code TEXT PRIMARY KEY,"
        "  name TEXT NOT NULL DEFAULT '',"
        "  kind TEXT NOT NULL DEFAULT '');"

        "CREATE TABLE IF NOT EXISTS model_attrs("
        "  model TEXT NOT NULL REFERENCES models(code) ON DELETE CASCADE,"
        "  key   TEXT NOT NULL,"
        "  value TEXT NOT NULL DEFAULT '',"
        "  PRIMARY KEY(model,key));"

        "CREATE TABLE IF NOT EXISTS model_points("
        "  model TEXT NOT NULL REFERENCES models(code) ON DELETE CASCADE,"
        "  key   TEXT NOT NULL,"
        "  name  TEXT NOT NULL DEFAULT '',"
        "  unit  TEXT NOT NULL DEFAULT '',"
        "  ord   INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(model,key));"

        // child_model 用 RESTRICT：被别的模型引用着就不许删，避免悬空组合
        "CREATE TABLE IF NOT EXISTS model_children("
        "  model       TEXT NOT NULL REFERENCES models(code) ON DELETE CASCADE,"
        "  child_model TEXT NOT NULL REFERENCES models(code) ON DELETE RESTRICT,"
        "  count       INTEGER NOT NULL DEFAULT 1,"
        "  ord         INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(model,ord));"

        "CREATE TABLE IF NOT EXISTS model_bindings("
        "  model        TEXT NOT NULL REFERENCES models(code) ON DELETE CASCADE,"
        "  point_key    TEXT NOT NULL,"
        "  task         TEXT NOT NULL DEFAULT '',"
        "  addr_formula TEXT NOT NULL DEFAULT '',"
        "  dtype        TEXT NOT NULL DEFAULT '',"
        "  scale        REAL NOT NULL DEFAULT 1.0,"
        "  offset       REAL NOT NULL DEFAULT 0.0,"
        "  raw_json     TEXT NOT NULL DEFAULT '',"
        "  PRIMARY KEY(model,point_key));"

        // ── 设备树 ──
        // id 为代理主键、path 为派生列：重命名只重写子树 path，
        // 引用（node_points / 编译快照）指向 id，不受影响。
        "CREATE TABLE IF NOT EXISTS tree_nodes("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  parent_id INTEGER REFERENCES tree_nodes(id) ON DELETE CASCADE,"
        "  ord       INTEGER NOT NULL DEFAULT 0,"
        "  code      TEXT NOT NULL,"
        "  name      TEXT NOT NULL DEFAULT '',"
        "  model     TEXT REFERENCES models(code) ON DELETE RESTRICT,"
        "  path      TEXT NOT NULL UNIQUE,"
        "  meta_json TEXT NOT NULL DEFAULT '{}');"
        "CREATE INDEX IF NOT EXISTS idx_nodes_parent ON tree_nodes(parent_id);"

        "CREATE TABLE IF NOT EXISTS node_points("
        "  node_id   INTEGER NOT NULL REFERENCES tree_nodes(id) ON DELETE CASCADE,"
        "  point_key TEXT NOT NULL,"
        "  task      TEXT NOT NULL DEFAULT '',"
        "  addr      TEXT NOT NULL DEFAULT '',"
        "  dtype     TEXT NOT NULL DEFAULT '',"
        "  scale     REAL NOT NULL DEFAULT 1.0,"
        "  offset    REAL NOT NULL DEFAULT 0.0,"
        "  raw_json  TEXT NOT NULL DEFAULT '',"
        "  ord       INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(node_id,point_key));"

        // ── 任务 / 通道 ──
        "CREATE TABLE IF NOT EXISTS tasks("
        "  id            TEXT PRIMARY KEY,"
        "  protocol      TEXT NOT NULL,"
        "  endpoint_json TEXT NOT NULL DEFAULT '{}',"
        "  interval_ms   INTEGER NOT NULL DEFAULT 1000,"
        "  enabled       INTEGER NOT NULL DEFAULT 1,"
        "  reconnect_sec INTEGER NOT NULL DEFAULT 5);"

        "CREATE TABLE IF NOT EXISTS channels("
        "  id          TEXT PRIMARY KEY,"
        "  type        TEXT NOT NULL,"
        "  direction   TEXT NOT NULL DEFAULT 'up',"
        "  config_json TEXT NOT NULL DEFAULT '{}',"
        "  enabled     INTEGER NOT NULL DEFAULT 1);"

        // ── 系统设置 ──
        "CREATE TABLE IF NOT EXISTS settings("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL DEFAULT '');"

        // ── 编译快照：运行期只读这里，编辑设备树不影响正在跑的采集 ──
        "CREATE TABLE IF NOT EXISTS config_versions("
        "  ver        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  created_at TEXT NOT NULL DEFAULT '',"
        "  comment    TEXT NOT NULL DEFAULT '');"

        "CREATE TABLE IF NOT EXISTS compiled_points("
        "  ver       INTEGER NOT NULL REFERENCES config_versions(ver) ON DELETE CASCADE,"
        "  task      TEXT NOT NULL,"
        "  node_path TEXT NOT NULL,"
        "  point_key TEXT NOT NULL,"
        "  addr      TEXT NOT NULL DEFAULT '',"
        "  dtype     TEXT NOT NULL DEFAULT '',"
        "  scale     REAL NOT NULL DEFAULT 1.0,"
        "  offset    REAL NOT NULL DEFAULT 0.0,"
        "  raw_json  TEXT NOT NULL DEFAULT '');"
        "CREATE INDEX IF NOT EXISTS idx_compiled_ver_task ON compiled_points(ver,task);";

    return exec(DDL);
}

int ConfigDb::schemaVersion() {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "SELECT COALESCE(MAX(ver),0) FROM schema_version;");
    if (!s) return 0;
    return s.step() ? (int)s.i64(0) : 0;
}

bool ConfigDb::migrateSchema() {
    int cur = schemaVersion();
    if (cur == CONFIG_DB_SCHEMA_VERSION) return true;
    if (cur > CONFIG_DB_SCHEMA_VERSION) {
        last_error_ = "配置库结构版本(v" + std::to_string(cur) +
                      ")高于本程序支持的 v" + std::to_string(CONFIG_DB_SCHEMA_VERSION) +
                      "，请升级程序，勿用旧版打开以免损坏配置";
        return false;
    }
    // cur < 目标：逐版升级。v0 → v1 即首次建库，createSchema 已完成。
    // 后续版本在此追加 if (cur < N) { ...ALTER...; cur = N; }
    std::lock_guard<std::mutex> lk(mtx_);
    return exec_locked(("INSERT OR REPLACE INTO schema_version(ver) VALUES(" +
                        std::to_string(CONFIG_DB_SCHEMA_VERSION) + ");").c_str());
}

// ── 事务 ─────────────────────────────────────────────────────────────────────
bool ConfigDb::begin()    { return exec("BEGIN IMMEDIATE;"); }
bool ConfigDb::commit()   { return exec("COMMIT;"); }
bool ConfigDb::rollback() { return exec("ROLLBACK;"); }

// ── 设备规格 ───────────────────────────────────────────────────────────────────

bool ConfigDb::upsertModel(const ModelRow& m) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "INSERT INTO models(code,name,kind) VALUES(?,?,?) "
                "ON CONFLICT(code) DO UPDATE SET name=excluded.name,kind=excluded.kind;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1, m.code); s.bindText(2, m.name); s.bindText(3, m.kind);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::addModelChild(const ModelChildRow& c) {
    // 成环则拒绝：编译器展开子树时假定组合关系是 DAG
    if (wouldCycle(c.model, c.child_model)) {
        last_error_ = "模型组合成环：" + c.model + " → " + c.child_model;
        return false;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "INSERT INTO model_children(model,child_model,count,ord) VALUES(?,?,?,?) "
                "ON CONFLICT(model,ord) DO UPDATE SET "
                "child_model=excluded.child_model,count=excluded.count;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1, c.model); s.bindText(2, c.child_model);
    s.bindInt(3, c.count);  s.bindInt(4, c.ord);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

// 自 child_model 向下遍历其组合子树，若能到达 model 则成环。
// 自环（model == child_model）在第一步即命中。
bool ConfigDb::wouldCycle(const std::string& model, const std::string& child_model) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return false;
    if (model == child_model) return true;

    std::vector<std::string> stack{child_model};
    std::vector<std::string> seen;
    while (!stack.empty()) {
        std::string cur = stack.back();
        stack.pop_back();
        if (cur == model) return true;
        bool dup = false;
        for (auto& s : seen) if (s == cur) { dup = true; break; }
        if (dup) continue;
        seen.push_back(cur);

        Stmt s(db_, "SELECT child_model FROM model_children WHERE model=?;");
        if (!s) break;
        s.bindText(1, cur);
        while (s.step()) stack.push_back(s.text(0));
    }
    return false;
}

bool ConfigDb::addModelPoint(const ModelPointRow& p) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "INSERT INTO model_points(model,key,name,unit,ord) VALUES(?,?,?,?,?) "
                "ON CONFLICT(model,key) DO UPDATE SET "
                "name=excluded.name,unit=excluded.unit,ord=excluded.ord;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1,p.model); s.bindText(2,p.key); s.bindText(3,p.name);
    s.bindText(4,p.unit);  s.bindInt(5,p.ord);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::addModelAttr(const ModelAttrRow& a) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "INSERT INTO model_attrs(model,key,value) VALUES(?,?,?) "
                "ON CONFLICT(model,key) DO UPDATE SET value=excluded.value;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1,a.model); s.bindText(2,a.key); s.bindText(3,a.value);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::upsertBinding(const BindingRow& b) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "INSERT INTO model_bindings"
                "(model,point_key,task,addr_formula,dtype,scale,offset,raw_json) "
                "VALUES(?,?,?,?,?,?,?,?) "
                "ON CONFLICT(model,point_key) DO UPDATE SET "
                "task=excluded.task,addr_formula=excluded.addr_formula,"
                "dtype=excluded.dtype,scale=excluded.scale,"
                "offset=excluded.offset,raw_json=excluded.raw_json;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1,b.model); s.bindText(2,b.point_key); s.bindText(3,b.task);
    s.bindText(4,b.addr_formula); s.bindText(5,b.dtype);
    s.bindReal(6,b.scale); s.bindReal(7,b.offset); s.bindText(8,b.raw_json);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

std::vector<ModelRow> ConfigDb::listModels() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ModelRow> out;
    Stmt s(db_, "SELECT code,name,kind FROM models ORDER BY code;");
    if (!s) return out;
    while (s.step()) out.push_back({s.text(0), s.text(1), s.text(2)});
    return out;
}

std::vector<ModelChildRow> ConfigDb::childrenOf(const std::string& model) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ModelChildRow> out;
    Stmt s(db_, "SELECT model,child_model,count,ord FROM model_children "
                "WHERE model=? ORDER BY ord;");
    if (!s) return out;
    s.bindText(1, model);
    while (s.step())
        out.push_back({s.text(0), s.text(1), (int)s.i64(2), (int)s.i64(3)});
    return out;
}

std::vector<ModelPointRow> ConfigDb::pointsOf(const std::string& model) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ModelPointRow> out;
    Stmt s(db_, "SELECT model,key,name,unit,ord FROM model_points "
                "WHERE model=? ORDER BY ord,key;");
    if (!s) return out;
    s.bindText(1, model);
    while (s.step())
        out.push_back({s.text(0), s.text(1), s.text(2), s.text(3), (int)s.i64(4)});
    return out;
}

std::vector<ModelAttrRow> ConfigDb::attrsOf(const std::string& model) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ModelAttrRow> out;
    Stmt s(db_, "SELECT model,key,value FROM model_attrs WHERE model=? ORDER BY key;");
    if (!s) return out;
    s.bindText(1, model);
    while (s.step()) out.push_back({s.text(0), s.text(1), s.text(2)});
    return out;
}

// ── 设备树 ───────────────────────────────────────────────────────────────────

std::string ConfigDb::pathOf_locked(int64_t node_id) {
    Stmt s(db_, "SELECT path FROM tree_nodes WHERE id=?;");
    if (!s) return {};
    s.bindInt(1, node_id);
    return s.step() ? s.text(0) : std::string();
}

std::optional<int64_t> ConfigDb::addNode(const std::optional<int64_t>& parent_id,
                                         const std::string& code,
                                         const std::string& name,
                                         const std::optional<std::string>& model,
                                         const std::string& meta_json) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return std::nullopt;
    if (code.empty()) { last_error_ = "节点 code 不能为空"; return std::nullopt; }
    // path 里用 '.' 分隔，code 自身含 '.' 会让路径无法反解
    if (code.find('.') != std::string::npos) {
        last_error_ = "节点 code 不能含 '.'：" + code;
        return std::nullopt;
    }

    std::string path = code;
    if (parent_id) {
        std::string ppath = pathOf_locked(*parent_id);
        if (ppath.empty()) { last_error_ = "父节点不存在"; return std::nullopt; }
        path = ppath + "." + code;
    }

    int ord = 0;
    {
        Stmt q(db_, parent_id
                    ? "SELECT COALESCE(MAX(ord),-1)+1 FROM tree_nodes WHERE parent_id=?;"
                    : "SELECT COALESCE(MAX(ord),-1)+1 FROM tree_nodes WHERE parent_id IS NULL;");
        if (q) {
            if (parent_id) q.bindInt(1, *parent_id);
            if (q.step()) ord = (int)q.i64(0);
        }
    }

    Stmt s(db_, "INSERT INTO tree_nodes(parent_id,ord,code,name,model,path,meta_json) "
                "VALUES(?,?,?,?,?,?,?);");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return std::nullopt; }
    if (parent_id) s.bindInt(1, *parent_id); else s.bindNull(1);
    s.bindInt(2, ord);
    s.bindText(3, code);
    s.bindText(4, name);
    if (model) s.bindText(5, *model); else s.bindNull(5);
    s.bindText(6, path);
    s.bindText(7, meta_json.empty() ? "{}" : meta_json);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return std::nullopt; }
    return sqlite3_last_insert_rowid(db_);
}

bool ConfigDb::addNodePoint(const NodePointRow& p) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "INSERT INTO node_points"
                "(node_id,point_key,task,addr,dtype,scale,offset,raw_json,ord) "
                "VALUES(?,?,?,?,?,?,?,?,?) "
                "ON CONFLICT(node_id,point_key) DO UPDATE SET "
                "task=excluded.task,addr=excluded.addr,dtype=excluded.dtype,"
                "scale=excluded.scale,offset=excluded.offset,"
                "raw_json=excluded.raw_json,ord=excluded.ord;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindInt(1,p.node_id);  s.bindText(2,p.point_key); s.bindText(3,p.task);
    s.bindText(4,p.addr);    s.bindText(5,p.dtype);
    s.bindReal(6,p.scale);   s.bindReal(7,p.offset);
    s.bindText(8,p.raw_json);s.bindInt(9,p.ord);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

std::vector<TreeNodeRow> ConfigDb::listNodes() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<TreeNodeRow> out;
    // 按 (parent_id, ord) 逐层还原插入次序；按 path 排序会把 meter_10 排在
    // meter_2 前面（字典序），导出后点位顺序与原配置不符。
    Stmt s(db_, "SELECT id,parent_id,ord,code,name,model,path,meta_json "
                "FROM tree_nodes ORDER BY parent_id IS NOT NULL, parent_id, ord;");
    if (!s) return out;
    while (s.step()) {
        TreeNodeRow r;
        r.id = s.i64(0);
        if (!s.isNull(1)) r.parent_id = s.i64(1);
        r.ord  = (int)s.i64(2);
        r.code = s.text(3);
        r.name = s.text(4);
        if (!s.isNull(5)) r.model = s.text(5);
        r.path      = s.text(6);
        r.meta_json = s.text(7);
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<std::pair<TreeNodeRow,int64_t>>
ConfigDb::listChildren(const std::optional<int64_t>& parent) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::pair<TreeNodeRow,int64_t>> out;
    // 子节点数用相关子查询一并取回，避免每个子节点再查一次（6000 节点会退化成
    // 上千次往返）。idx_nodes_parent 索引使其保持廉价。
    const char* SQL = parent
        ? "SELECT n.id,n.parent_id,n.ord,n.code,n.name,n.model,n.path,n.meta_json,"
          "  (SELECT COUNT(*) FROM tree_nodes c WHERE c.parent_id=n.id) "
          "FROM tree_nodes n WHERE n.parent_id=? ORDER BY n.ord;"
        : "SELECT n.id,n.parent_id,n.ord,n.code,n.name,n.model,n.path,n.meta_json,"
          "  (SELECT COUNT(*) FROM tree_nodes c WHERE c.parent_id=n.id) "
          "FROM tree_nodes n WHERE n.parent_id IS NULL ORDER BY n.ord;";
    Stmt s(db_, SQL);
    if (!s) return out;
    if (parent) s.bindInt(1, *parent);
    while (s.step()) {
        TreeNodeRow r;
        r.id = s.i64(0);
        if (!s.isNull(1)) r.parent_id = s.i64(1);
        r.ord = (int)s.i64(2); r.code = s.text(3); r.name = s.text(4);
        if (!s.isNull(5)) r.model = s.text(5);
        r.path = s.text(6); r.meta_json = s.text(7);
        out.emplace_back(std::move(r), s.i64(8));
    }
    return out;
}

std::vector<NodePointRow> ConfigDb::nodePoints(int64_t node_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<NodePointRow> out;
    Stmt s(db_, "SELECT node_id,point_key,task,addr,dtype,scale,offset,raw_json,ord "
                "FROM node_points WHERE node_id=? ORDER BY ord;");
    if (!s) return out;
    s.bindInt(1, node_id);
    while (s.step()) {
        NodePointRow r;
        r.node_id  = s.i64(0);  r.point_key = s.text(1); r.task = s.text(2);
        r.addr     = s.text(3); r.dtype     = s.text(4);
        r.scale    = s.real(5); r.offset    = s.real(6);
        r.raw_json = s.text(7); r.ord       = (int)s.i64(8);
        out.push_back(std::move(r));
    }
    return out;
}

bool ConfigDb::deleteNode(int64_t node_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "DELETE FROM tree_nodes WHERE id=?;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindInt(1, node_id);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::deleteModel(const std::string& code) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "DELETE FROM models WHERE code=?;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1, code);
    // 仍被设备树引用时，RESTRICT 使这里返回约束错误 —— 正是我们要的
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

// 把 node 及其整棵子树的 path 前缀由 oldPrefix 换成 newPrefix。
// 用 substr 定位而非 LIKE：code 里可能含 '%' 或 '_'，那是 LIKE 的通配符，
// 不转义会误伤无关节点。
bool ConfigDb::rewriteSubtreePaths_locked(const std::string& oldPrefix,
                                          const std::string& newPrefix) {
    // 本节点
    {
        Stmt s(db_, "UPDATE tree_nodes SET path=? WHERE path=?;");
        if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
        s.bindText(1, newPrefix); s.bindText(2, oldPrefix);
        if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    }
    // 子孙：path 以 "oldPrefix." 打头的那些
    const std::string oldDot = oldPrefix + ".";
    Stmt s(db_, "UPDATE tree_nodes SET path = ? || substr(path, ?) "
                "WHERE substr(path, 1, ?) = ?;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1, newPrefix + ".");
    s.bindInt (2, (int64_t)oldDot.size() + 1);   // substr 从 1 开始计
    s.bindInt (3, (int64_t)oldDot.size());
    s.bindText(4, oldDot);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::renameNode(int64_t node_id, const std::string& new_code) {
    if (new_code.empty() || new_code.find('.') != std::string::npos) {
        last_error_ = "节点 code 非法（不能为空或含 '.'）：" + new_code;
        return false;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return false;

    std::string oldPath;
    std::optional<int64_t> parent;
    {
        Stmt s(db_, "SELECT path,parent_id FROM tree_nodes WHERE id=?;");
        if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
        s.bindInt(1, node_id);
        if (!s.step()) { last_error_ = "节点不存在"; return false; }
        oldPath = s.text(0);
        if (!s.isNull(1)) parent = s.i64(1);
    }
    std::string newPath = new_code;
    if (parent) {
        std::string ppath = pathOf_locked(*parent);
        if (ppath.empty()) { last_error_ = "父节点不存在"; return false; }
        newPath = ppath + "." + new_code;
    }
    if (newPath == oldPath) return true;

    if (!exec_locked("SAVEPOINT rn;")) return false;
    Stmt s(db_, "UPDATE tree_nodes SET code=? WHERE id=?;");
    if (!s) { exec_locked("ROLLBACK TO rn; RELEASE rn;"); return false; }
    s.bindText(1, new_code); s.bindInt(2, node_id);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_);
                     exec_locked("ROLLBACK TO rn; RELEASE rn;"); return false; }
    // path UNIQUE 会在此挡下同父重名
    if (!rewriteSubtreePaths_locked(oldPath, newPath)) {
        exec_locked("ROLLBACK TO rn; RELEASE rn;");
        return false;
    }
    return exec_locked("RELEASE rn;");
}

bool ConfigDb::moveNode(int64_t node_id, const std::optional<int64_t>& new_parent) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return false;

    std::string oldPath, code;
    {
        Stmt s(db_, "SELECT path,code FROM tree_nodes WHERE id=?;");
        if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
        s.bindInt(1, node_id);
        if (!s.step()) { last_error_ = "节点不存在"; return false; }
        oldPath = s.text(0); code = s.text(1);
    }

    std::string newPath = code;
    if (new_parent) {
        if (*new_parent == node_id) { last_error_ = "不能把节点移到自己下面"; return false; }
        std::string ppath = pathOf_locked(*new_parent);
        if (ppath.empty()) { last_error_ = "目标父节点不存在"; return false; }
        // 移到自己的子孙下会让这棵子树脱离根，成为自引用环
        if (ppath.size() > oldPath.size() &&
            ppath.compare(0, oldPath.size() + 1, oldPath + ".") == 0) {
            last_error_ = "不能把节点移到它自己的子孙下";
            return false;
        }
        newPath = ppath + "." + code;
    }
    if (newPath == oldPath) return true;

    if (!exec_locked("SAVEPOINT mv;")) return false;
    {
        Stmt s(db_, "UPDATE tree_nodes SET parent_id=? WHERE id=?;");
        if (!s) { exec_locked("ROLLBACK TO mv; RELEASE mv;"); return false; }
        if (new_parent) s.bindInt(1, *new_parent); else s.bindNull(1);
        s.bindInt(2, node_id);
        if (!s.done()) { last_error_ = sqlite3_errmsg(db_);
                         exec_locked("ROLLBACK TO mv; RELEASE mv;"); return false; }
    }
    if (!rewriteSubtreePaths_locked(oldPath, newPath)) {
        exec_locked("ROLLBACK TO mv; RELEASE mv;");
        return false;
    }
    return exec_locked("RELEASE mv;");
}

bool ConfigDb::updateNode(int64_t node_id, const std::string& name,
                          const std::optional<std::string>& model) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "UPDATE tree_nodes SET name=?, model=? WHERE id=?;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1, name);
    if (model) s.bindText(2, *model); else s.bindNull(2);
    s.bindInt(3, node_id);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::updateNodeMeta(int64_t node_id, const std::string& meta_json) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "UPDATE tree_nodes SET meta_json=? WHERE id=?;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1, meta_json.empty() ? "{}" : meta_json);
    s.bindInt(2, node_id);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::deleteNodePoint(int64_t node_id, const std::string& point_key) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "DELETE FROM node_points WHERE node_id=? AND point_key=?;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindInt(1, node_id); s.bindText(2, point_key);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::deleteTask(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "DELETE FROM tasks WHERE id=?;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1, id);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::deleteChannel(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "DELETE FROM channels WHERE id=?;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1, id);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::deleteModelChild(const std::string& model, int ord) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "DELETE FROM model_children WHERE model=? AND ord=?;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1, model); s.bindInt(2, ord);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::deleteModelPoint(const std::string& model, const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "DELETE FROM model_points WHERE model=? AND key=?;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1, model); s.bindText(2, key);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::appendModelChild(const std::string& model,
                                const std::string& child_model, int count) {
    int nextOrd = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Stmt s(db_, "SELECT COALESCE(MAX(ord),-1)+1 FROM model_children WHERE model=?;");
        if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
        s.bindText(1, model);
        if (s.step()) nextOrd = (int)s.i64(0);
    }
    return addModelChild({model, child_model, count, nextOrd});
}

// ── 实例化 ───────────────────────────────────────────────────────────────────

// 只数不建。组合已由 wouldCycle 保证无环，但深度仍设上限以防病态深模型。
int64_t ConfigDb::countInstanceNodes(const std::string& model, int depth_cap) {
    if (depth_cap <= 0) return -1;          // 过深，视为非法
    int64_t total = 1;                       // 自身
    for (const auto& c : childrenOf(model)) {
        int64_t sub = countInstanceNodes(c.child_model, depth_cap - 1);
        if (sub < 0) return -1;
        // 溢出保护：病态模型（如 100×100×100…）会让乘积爆掉 int64
        if (c.count > 0 && sub > (INT64_MAX - total) / c.count) return -1;
        total += (int64_t)c.count * sub;
    }
    return total;
}

namespace {
// 实例前缀：kind 优先（语义上就是给这个用的），否则用小写型号
std::string instancePrefix(const std::string& kind, const std::string& code) {
    if (!kind.empty()) return kind;
    std::string s = code;
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}
} // namespace

bool ConfigDb::instantiateModel(const std::optional<int64_t>& parent_id,
                                const std::string& code, const std::string& name,
                                const std::string& model, int64_t max_nodes,
                                int64_t& created, std::string& err) {
    if (!ok()) { err = "配置库不可用"; return false; }

    // 先估规模 —— 拒绝要在动库之前
    const int64_t n = countInstanceNodes(model);
    if (n < 0) {
        err = "设备规格 " + model + " 组合过深或规模溢出，无法展开";
        return false;
    }
    if (n > max_nodes) {
        err = "展开 " + model + " 需创建 " + std::to_string(n) +
              " 个节点，超过上限 " + std::to_string(max_nodes) +
              "（可提高上限，但请先确认模型组合数量填写无误）";
        return false;
    }

    // 型号 → (kind, 有无子规格) 的缓存，免得每层重复查库
    std::map<std::string, std::string> kindOf;
    for (const auto& m : listModels()) kindOf[m.code] = m.kind;
    if (kindOf.find(model) == kindOf.end()) {
        err = "设备规格不存在: " + model;
        return false;
    }

    if (!begin()) { err = "开启事务失败: " + lastError(); return false; }
    created = 0;

    // 递归展开。层数已由 countInstanceNodes 的 depth_cap 限住，不会爆栈。
    std::function<bool(const std::optional<int64_t>&, const std::string&,
                       const std::string&, const std::string&)> expand =
    [&](const std::optional<int64_t>& parent, const std::string& c,
        const std::string& nm, const std::string& mdl) -> bool {
        auto id = addNode(parent, c, nm, mdl);
        if (!id) { err = "创建节点 " + c + " 失败: " + lastError(); return false; }
        ++created;

        for (const auto& ch : childrenOf(mdl)) {
            auto kit = kindOf.find(ch.child_model);
            const std::string pfx = instancePrefix(
                kit == kindOf.end() ? std::string() : kit->second, ch.child_model);
            // 序号宽度按数量定：24 个电芯是 cell01..cell24，不是 cell1..cell24，
            // 这样 CSV 在 Excel 里按名字排序才不会把 cell10 排到 cell2 前面
            const int width = (int)std::to_string(ch.count).size();
            for (int i = 1; i <= ch.count; ++i) {
                std::string idx = std::to_string(i);
                while ((int)idx.size() < width) idx = "0" + idx;
                if (!expand(*id, pfx + idx, "", ch.child_model)) return false;
            }
        }
        return true;
    };

    if (!expand(parent_id, code, name, model)) { rollback(); return false; }
    if (!commit()) { rollback(); err = "提交失败: " + lastError(); return false; }
    return true;
}

std::optional<TreeNodeRow> ConfigDb::nodeByPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "SELECT id,parent_id,ord,code,name,model,path,meta_json "
                "FROM tree_nodes WHERE path=?;");
    if (!s) return std::nullopt;
    s.bindText(1, path);
    if (!s.step()) return std::nullopt;
    TreeNodeRow r;
    r.id = s.i64(0);
    if (!s.isNull(1)) r.parent_id = s.i64(1);
    r.ord = (int)s.i64(2); r.code = s.text(3); r.name = s.text(4);
    if (!s.isNull(5)) r.model = s.text(5);
    r.path      = s.text(6);
    r.meta_json = s.text(7);
    return r;
}

// ── 任务 / 通道 ──────────────────────────────────────────────────────────────

bool ConfigDb::upsertTask(const TaskRow& t) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "INSERT INTO tasks(id,protocol,endpoint_json,interval_ms,enabled,reconnect_sec) "
                "VALUES(?,?,?,?,?,?) "
                "ON CONFLICT(id) DO UPDATE SET protocol=excluded.protocol,"
                "endpoint_json=excluded.endpoint_json,interval_ms=excluded.interval_ms,"
                "enabled=excluded.enabled,reconnect_sec=excluded.reconnect_sec;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1,t.id); s.bindText(2,t.protocol); s.bindText(3,t.endpoint_json);
    s.bindInt(4,t.interval_ms); s.bindInt(5,t.enabled?1:0); s.bindInt(6,t.reconnect_sec);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool ConfigDb::upsertChannel(const ChannelRow& c) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "INSERT INTO channels(id,type,direction,config_json,enabled) "
                "VALUES(?,?,?,?,?) "
                "ON CONFLICT(id) DO UPDATE SET type=excluded.type,"
                "direction=excluded.direction,config_json=excluded.config_json,"
                "enabled=excluded.enabled;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1,c.id); s.bindText(2,c.type); s.bindText(3,c.direction);
    s.bindText(4,c.config_json); s.bindInt(5,c.enabled?1:0);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

std::vector<TaskRow> ConfigDb::listTasks() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<TaskRow> out;
    Stmt s(db_, "SELECT id,protocol,endpoint_json,interval_ms,enabled,reconnect_sec "
                "FROM tasks ORDER BY rowid;");
    if (!s) return out;
    while (s.step())
        out.push_back({s.text(0), s.text(1), s.text(2),
                       (int)s.i64(3), s.i64(4)!=0, (int)s.i64(5)});
    return out;
}

std::vector<ChannelRow> ConfigDb::listChannels() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ChannelRow> out;
    Stmt s(db_, "SELECT id,type,direction,config_json,enabled FROM channels ORDER BY rowid;");
    if (!s) return out;
    while (s.step())
        out.push_back({s.text(0), s.text(1), s.text(2), s.text(3), s.i64(4)!=0});
    return out;
}

// ── 系统设置 ─────────────────────────────────────────────────────────────────

bool ConfigDb::setSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "INSERT INTO settings(key,value) VALUES(?,?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value;");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindText(1,key); s.bindText(2,value);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

std::string ConfigDb::getSetting(const std::string& key, const std::string& def) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "SELECT value FROM settings WHERE key=?;");
    if (!s) return def;
    s.bindText(1, key);
    return s.step() ? s.text(0) : def;
}

std::vector<std::pair<std::string,std::string>> ConfigDb::allSettings() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::pair<std::string,std::string>> out;
    Stmt s(db_, "SELECT key,value FROM settings ORDER BY key;");
    if (!s) return out;
    while (s.step()) out.emplace_back(s.text(0), s.text(1));
    return out;
}

// ── 编译快照 ─────────────────────────────────────────────────────────────────

std::vector<BindingRow> ConfigDb::bindingsOf(const std::string& model) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<BindingRow> out;
    Stmt s(db_, "SELECT model,point_key,task,addr_formula,dtype,scale,offset,raw_json "
                "FROM model_bindings WHERE model=? ORDER BY point_key;");
    if (!s) return out;
    s.bindText(1, model);
    while (s.step()) {
        BindingRow b;
        b.model = s.text(0); b.point_key = s.text(1); b.task = s.text(2);
        b.addr_formula = s.text(3); b.dtype = s.text(4);
        b.scale = s.real(5); b.offset = s.real(6); b.raw_json = s.text(7);
        out.push_back(std::move(b));
    }
    return out;
}

std::optional<int64_t> ConfigDb::newVersion(const std::string& comment) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "INSERT INTO config_versions(created_at,comment) VALUES(?,?);");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return std::nullopt; }
    // 时间戳用 UTC ISO8601，与缓存库的 ts 列保持同一形式
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    gmtime_r(&t, &tmv);
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    s.bindText(1, buf);
    s.bindText(2, comment);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return std::nullopt; }
    return sqlite3_last_insert_rowid(db_);
}

bool ConfigDb::addCompiledPoint(int64_t ver, const std::string& task,
                                const std::string& node_path, const std::string& point_key,
                                const std::string& addr, const std::string& dtype,
                                double scale, double offset, const std::string& raw_json) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "INSERT INTO compiled_points"
                "(ver,task,node_path,point_key,addr,dtype,scale,offset,raw_json) "
                "VALUES(?,?,?,?,?,?,?,?,?);");
    if (!s) { last_error_ = sqlite3_errmsg(db_); return false; }
    s.bindInt(1,ver); s.bindText(2,task); s.bindText(3,node_path);
    s.bindText(4,point_key); s.bindText(5,addr); s.bindText(6,dtype);
    s.bindReal(7,scale); s.bindReal(8,offset); s.bindText(9,raw_json);
    if (!s.done()) { last_error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

int64_t ConfigDb::latestVersion() {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "SELECT COALESCE(MAX(ver),0) FROM config_versions;");
    if (!s) return 0;
    return s.step() ? s.i64(0) : 0;
}

std::vector<ConfigDb::CompiledRow>
ConfigDb::compiledPoints(int64_t ver, const std::string& task) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<CompiledRow> out;
    Stmt s(db_, task.empty()
        ? "SELECT task,node_path,point_key,addr,dtype,scale,offset,raw_json "
          "FROM compiled_points WHERE ver=? ORDER BY rowid;"
        : "SELECT task,node_path,point_key,addr,dtype,scale,offset,raw_json "
          "FROM compiled_points WHERE ver=? AND task=? ORDER BY rowid;");
    if (!s) return out;
    s.bindInt(1, ver);
    if (!task.empty()) s.bindText(2, task);
    while (s.step()) {
        CompiledRow r;
        r.task = s.text(0); r.node_path = s.text(1); r.point_key = s.text(2);
        r.addr = s.text(3); r.dtype = s.text(4);
        r.scale = s.real(5); r.offset = s.real(6); r.raw_json = s.text(7);
        out.push_back(std::move(r));
    }
    return out;
}

bool ConfigDb::pruneVersions(int keep) {
    if (keep < 1) keep = 1;
    std::lock_guard<std::mutex> lk(mtx_);
    // compiled_points 靠 ON DELETE CASCADE 跟着删（PRAGMA foreign_keys=ON 已开）
    std::string sql = "DELETE FROM config_versions WHERE ver NOT IN "
                      "(SELECT ver FROM config_versions ORDER BY ver DESC LIMIT " +
                      std::to_string(keep) + ");";
    return exec_locked(sql.c_str());
}

int64_t ConfigDb::countRows(const std::string& table) {
    std::lock_guard<std::mutex> lk(mtx_);
    // 表名不能参数化绑定；仅接受白名单内的固定表名，杜绝拼接注入
    static const char* ALLOWED[] = {
        "models","model_attrs","model_points","model_children","model_bindings",
        "tree_nodes","node_points","tasks","channels","settings",
        "config_versions","compiled_points"};
    bool ok = false;
    for (auto t : ALLOWED) if (table == t) { ok = true; break; }
    if (!ok) { last_error_ = "未知表名: " + table; return -1; }

    Stmt s(db_, ("SELECT COUNT(*) FROM " + table + ";").c_str());
    if (!s) return -1;
    return s.step() ? s.i64(0) : 0;
}

} // namespace industrial
