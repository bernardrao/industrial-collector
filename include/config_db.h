#pragma once
/**
 * config_db.h — 配置库（config.db）存储层
 *
 * 与数据缓存（cache.db）严格分离，两库互不引用：
 *   config.db  低频写、强事务。设备规格 / 设备树 / 采集任务 / 通道 / 系统设置 /
 *              编译快照。跟随部署走 —— 备份即拷贝这一个文件。
 *   cache.db   高频写、WAL。采集数据与补传游标。可随时删除重建，删掉只丢
 *              未补传数据，不伤配置。
 *
 * ── 数据模型 ────────────────────────────────────────────────────────────────
 *   设备规格(models)      模板。可递归组合：model_children 声明"本模型由 N 个
 *                       子规格组成"，结构化存储而非把组成编码进名字 —— 名字
 *                       形如 PACK001.CELL001024 仅作显示，靠解析字符串还原
 *                       结构在型号名含数字或数量超 999 时会出歧义。
 *   设备树(tree_nodes)  设备规格的实例化。id 为代理主键，path 为派生列：
 *                       重命名节点只需重写子树的 path，所有引用（覆盖点、
 *                       编译快照）指向 id 不受影响。path 用于 MQTT 主题、
 *                       CSV 导出等对外表示。
 *   采集任务(tasks)     一个连接 + 一个协议 + 一个调度周期 = 一个线程。
 *   通道(channels)      MQTT / Kafka / 文件，上行下行独立启停。
 *   编译快照(compiled_*) "生效"动作的产物。运行期只读快照，编辑不影响运行。
 *
 * ── 无损往返 ────────────────────────────────────────────────────────────────
 * 点位除抽取可查询列（addr/dtype/scale）外，另存完整原始 JSON(raw_json)。
 * 迁移与导出因此是构造性无损的 —— 逐字段映射正是迁移器静默丢字段的根源。
 *
 * 线程安全：单 sqlite3* + 内部 mutex。
 */
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace industrial {

// 当前库结构版本。每次 schema 变更 +1 并在 migrateSchema() 中补一段升级逻辑。
constexpr int CONFIG_DB_SCHEMA_VERSION = 1;

// ── 行结构 ───────────────────────────────────────────────────────────────────

struct ModelRow {
    std::string code;          // 型号，如 CELL001 / PACK001
    std::string name;
    std::string kind;          // 自由分类：cell / pack / cluster / controller ...
};

struct ModelChildRow {
    std::string model;         // 父模型
    std::string child_model;
    int         count = 1;
    int         ord   = 0;     // 组合顺序，决定 idx_* 变量的编号次序
};

struct ModelPointRow {
    std::string model;
    std::string key;
    std::string name;
    std::string unit;
    int         ord = 0;
};

struct ModelAttrRow {
    std::string model;
    std::string key;
    std::string value;
};

// 协议绑定：地址是表达式，可引用各级祖先序号（idx_PACK / idx_CELL ...）
struct BindingRow {
    std::string model;
    std::string point_key;
    std::string task;
    std::string addr_formula;
    std::string dtype;
    double      scale  = 1.0;
    double      offset = 0.0;
    std::string raw_json;      // 完整原始定义，保证无损
};

struct TreeNodeRow {
    int64_t                id        = 0;    // 代理主键
    std::optional<int64_t> parent_id;        // 空 = 根
    int                    ord       = 0;
    std::string            code;             // 本级名，如 cell17
    std::string            name;
    std::optional<std::string> model;        // 空 = 自由节点（手填采集参数）
    std::string            path;             // 派生：station.rack01.clst03.cell17
    // 节点级元数据（非点位）。如 DLT 电表的 meter_address —— 它属于这块表本身，
    // 不是它的某个测点。留作 JSON 以免每来一种协议就加一列。
    std::string            meta_json = "{}";
};

// 节点上的点位：自由节点的全部点位，或模型实例上被手工覆盖的点位
struct NodePointRow {
    int64_t     node_id = 0;
    std::string point_key;
    std::string task;
    std::string addr;
    std::string dtype;
    double      scale  = 1.0;
    double      offset = 0.0;
    std::string raw_json;      // 完整原始点位 JSON，保证无损往返
    int         ord    = 0;
};

struct TaskRow {
    std::string id;
    std::string protocol;      // modbus / iec104 / iec61850 / opcua / dlt645 / dlt698 / can
    std::string endpoint_json; // 连接参数（协议块去掉点位容器后的剩余部分）
    int         interval_ms   = 1000;
    bool        enabled       = true;
    int         reconnect_sec = 5;
};

struct ChannelRow {
    std::string id;
    std::string type;          // mqtt / kafka / file
    std::string direction;     // up / down / both
    std::string config_json;
    bool        enabled = true;
};

// ── 存储层 ───────────────────────────────────────────────────────────────────

class ConfigDb {
public:
    explicit ConfigDb(const std::string& db_path);
    ~ConfigDb();

    bool ok() const { return db_ != nullptr; }
    const std::string& lastError() const { return last_error_; }

    // ── 事务。导入/迁移/生效都应包在一个事务里，失败整体回滚。 ──
    bool begin();
    bool commit();
    bool rollback();

    // ── 设备规格 ──
    bool upsertModel(const ModelRow& m);
    bool addModelChild(const ModelChildRow& c);
    bool addModelPoint(const ModelPointRow& p);
    bool addModelAttr(const ModelAttrRow& a);
    bool upsertBinding(const BindingRow& b);
    std::vector<ModelRow>      listModels();
    std::vector<ModelChildRow> childrenOf(const std::string& model);
    std::vector<ModelPointRow> pointsOf(const std::string& model);
    std::vector<ModelAttrRow>  attrsOf(const std::string& model);
    std::vector<BindingRow>    bindingsOf(const std::string& model);

    // 组合环检测：把 child 挂到 model 下是否会成环（含 model==child 自环）。
    // model_children 必须是 DAG —— 编译器展开子树时假定其有限，成环即无限展开。
    bool wouldCycle(const std::string& model, const std::string& child_model);

    // ── 设备树 ──
    // 追加子节点，自动计算 path。parent 为空表示根节点。
    std::optional<int64_t> addNode(const std::optional<int64_t>& parent_id,
                                   const std::string& code,
                                   const std::string& name,
                                   const std::optional<std::string>& model,
                                   const std::string& meta_json = "{}");
    bool addNodePoint(const NodePointRow& p);
    std::vector<TreeNodeRow>  listNodes();                  // 按层级+ord 还原插入序

    // 只取直接子节点，附带各自的子节点数（供前端判断能否展开）。
    // 设备树按需展开必须走这个：一个储能站 6000+ 节点，listNodes() 一次性
    // 下发约 0.9MB、前端再渲染成扁平 DOM，页面就废了。
    std::vector<std::pair<TreeNodeRow,int64_t>>
        listChildren(const std::optional<int64_t>& parent);
    std::vector<NodePointRow> nodePoints(int64_t node_id);  // 按 ord 排序
    std::optional<TreeNodeRow> nodeByPath(const std::string& path);

    // 删除节点。其子树与挂载的点位靠 ON DELETE CASCADE 一并删除。
    bool deleteNode(int64_t node_id);
    // 删除设备规格。被设备树实例化中的模型会被 RESTRICT 挡下（返回 false）。
    bool deleteModel(const std::string& code);

    // 改名 / 移动：重写本节点及【整棵子树】的 path。
    // 引用节点的表（node_points / 编译快照）都指向 id，故不受影响 —— 这正是
    // 当初选代理主键而非以 path 为身份的理由。
    // 同父下重名、移动到自己的子孙下（成环）都会被拒绝。
    bool renameNode(int64_t node_id, const std::string& new_code);
    bool moveNode(int64_t node_id, const std::optional<int64_t>& new_parent);
    bool updateNode(int64_t node_id, const std::string& name,
                    const std::optional<std::string>& model);
    bool updateNodeMeta(int64_t node_id, const std::string& meta_json);
    bool deleteNodePoint(int64_t node_id, const std::string& point_key);

    bool deleteTask(const std::string& id);
    bool deleteChannel(const std::string& id);
    bool deleteModelChild(const std::string& model, int ord);
    bool deleteModelPoint(const std::string& model, const std::string& key);

    // 追加子规格，ord 自动取当前最大值 +1。
    // 直接用 addModelChild 而不递增 ord 会因 ON CONFLICT(model,ord) 静默替换
    // 既有兄弟项 —— RACK002(7×CLST001 + 1×CTL001) 正是会踩中的形状。
    bool appendModelChild(const std::string& model, const std::string& child_model,
                          int count);

    // ── 实例化：把设备规格展开成设备树子树 ──────────────────────────────────
    //
    // 展开是【指数级】的：RACK001 = 8 簇 × 4 包 × 24 芯 = 768 个电芯节点，
    // 一个电站 8 台就是 6000+ 节点。故：
    //   · 先只数不建（countInstanceNodes），超过 max_nodes 直接拒绝 —— 建到一半
    //     再失败会留下一棵半成品树，比彻底不建更难收拾；
    //   · 整个展开在一个事务内完成 —— 逐条自动提交慢上百倍。
    //
    // 子节点命名：前缀取子规格的 kind（没有则取小写型号），序号按 count 宽度补零，
    // 如 8 个 CLST001(kind=cluster) → cluster1..cluster8。
    //
    // 节点只携带 model 引用，【不】逐个复制模型的测点 —— 5760 个电芯 × 2 点 =
    // 11520 行全部可由模型推导，落库纯属冗余。测点在 P2 编译时按公式展开。
    int64_t countInstanceNodes(const std::string& model, int depth_cap = 12);

    bool instantiateModel(const std::optional<int64_t>& parent_id,
                          const std::string& code, const std::string& name,
                          const std::string& model, int64_t max_nodes,
                          int64_t& created, std::string& err);

    // ── 采集任务 / 通道 ──
    bool upsertTask(const TaskRow& t);
    bool upsertChannel(const ChannelRow& c);
    std::vector<TaskRow>    listTasks();
    std::vector<ChannelRow> listChannels();

    // ── 系统设置（web / cache / logging，扁平 key-value）──
    bool        setSetting(const std::string& key, const std::string& value);
    std::string getSetting(const std::string& key, const std::string& def = "");
    std::vector<std::pair<std::string,std::string>> allSettings();

    // ── 编译快照 ──
    // 保留最近 keep 个版本 —— 万级点位场景下每版本上万行，无限保留会撑爆库。
    bool pruneVersions(int keep);

    // 新建一个版本，返回版本号
    std::optional<int64_t> newVersion(const std::string& comment);
    // 往指定版本追加一条编译点（调用方应包在事务里，逐条自动提交会慢上百倍）
    bool addCompiledPoint(int64_t ver, const std::string& task,
                          const std::string& node_path, const std::string& point_key,
                          const std::string& addr, const std::string& dtype,
                          double scale, double offset, const std::string& raw_json);
    // 最新版本号；无任何版本时返回 0
    int64_t latestVersion();
    // 取某版本某任务的全部编译点（运行期采集线程按任务取自己的点表）
    struct CompiledRow {
        std::string task, node_path, point_key, addr, dtype, raw_json;
        double scale = 1.0, offset = 0.0;
    };
    std::vector<CompiledRow> compiledPoints(int64_t ver, const std::string& task = "");

    int64_t countRows(const std::string& table);

private:
    sqlite3*           db_ = nullptr;
    mutable std::mutex mtx_;
    std::string        last_error_;

    bool exec(const char* sql);
    bool createSchema();
    bool migrateSchema();
    int  schemaVersion();

    // 调用方已持锁
    bool exec_locked(const char* sql);
    std::string pathOf_locked(int64_t node_id);
    bool rewriteSubtreePaths_locked(const std::string& oldPrefix,
                                    const std::string& newPrefix);
};

// ── config.json → config.db 迁移 ─────────────────────────────────────────────
//
// 在 JSON 层面工作而非逐字段映射：每个设备的协议块拆成"连接参数(endpoint_json)"
// 与"点位容器"，点位原样保存 raw_json。DLT645/698 的总线-电表两级结构天然映射
// 为"任务 + 子节点"，正好印证两级设计。
//
// 返回 false 时 err 内含原因，且不会留下半成品（整体事务回滚）。
struct MigrateStats {
    int tasks = 0, nodes = 0, points = 0, channels = 0, settings = 0;
};
bool migrateJsonToDb(const std::string& json_text, ConfigDb& db,
                     MigrateStats& stats, std::string& err);

// config.db → config.json（导出/备份/穿网闸摆渡）。与上者互为逆运算，
// 二者的往返一致性由 config_db_test 的语义往返测试守护。
bool exportDbToJson(ConfigDb& db, std::string& json_text, std::string& err);

// ── 编译（「生效」）────────────────────────────────────────────────────────
//
// 遍历设备树，把"设备规格 + 地址公式"展开成每个实例的具体测点，写入一个新的
// 编译快照版本。运行期只读快照 —— 编辑设备树不影响正在跑的采集，只有「生效」
// 才切换版本。
//
// 公式变量：本级序号 idx，以及各级祖先按其设备规格 code 命名的 idx_<CODE>。
// 电芯地址同时取决于它在包内的序号与包在簇内的序号，只给 sibling_id 不够用。
//
// 自由节点（无设备规格）的 node_points 原样进快照，地址已是字面量无需求值。
// 模型实例上若某个测点被 node_points 覆盖，以覆盖为准。
struct CompileStats {
    int64_t nodes = 0;        // 参与编译的节点
    int64_t points = 0;       // 产出的测点
    int64_t from_model = 0;   // 其中由设备规格公式展开的
    int64_t overridden = 0;   // 其中被节点覆盖的
    int     warnings = 0;     // 非致命问题（如模型有测点却无绑定）
};

// 成功时 ver 为新版本号。任何一个公式求值失败都会导致整体失败并回滚 ——
// 一半地址正确、一半悬空的快照比编译失败危险得多。
bool compileTree(ConfigDb& db, int64_t& ver, CompileStats& stats,
                 std::vector<std::string>& warnings, std::string& err);

// ── 编译快照 → 运行时设备表 ────────────────────────────────────────────────
//
// 把 tasks + compiled_points 还原成 config.json 里 devices[] 的形状，交给既有的
// appConfigFromJsonString 解析。七个采集器因此一行都不用改 —— 风险集中在这一个
// 可离线验证的转换函数里，而不是散进现场跑了很久的采集代码。
struct RuntimeStats {
    int64_t tasks = 0, points = 0, meters = 0;
    int64_t disabled_tasks = 0, empty_tasks = 0;
};
bool buildRuntimeJson(ConfigDb& db, int64_t ver, std::string& json_text,
                      RuntimeStats& stats, std::string& err);

// ── 设备树 ⇄ CSV（供 Excel 批量编辑）────────────────────────────────────────
// 每点位一行；无点位的节点也占一行（point_key 留空），否则往返会丢节点。
// 导入是全量替换：先全量校验，再在一个事务里重建，失败不留半成品。
bool exportTreeToCsv(ConfigDb& db, std::string& csv, std::string& err);
bool importTreeFromCsv(const std::string& csv, ConfigDb& db,
                       int& nodes_out, int& points_out, std::string& err);

} // namespace industrial

// 模型驱动板块的 HTTP 路由注册（实现在 src/web_api_model.cpp）。
// 放在 industrial 命名空间外的前置声明会牵进 httplib.h，故仍留在命名空间内，
// 但只在包含了 httplib 的翻译单元里可见。
namespace httplib { class Server; }
namespace industrial {

// 「生效」编译出新版本后的热切换回调。由 main 提供（它才持有 SharedState 与
// 采集线程池），Web 层只负责在编译成功后调用一次。
//   返回 true  = 已按 msg 说明处理妥当（热切换成功，或采集源本就不是库因而无需切换）
//   返回 false = 热切换失败，msg 说明原因；【新版本仍已写入库】，重启即可生效。
// 之所以要区分这两者：/api/compile 走到这里时版本已经落库，前端必须能把
// "编译成功但没换上去"如实告诉用户，而不是笼统地报一句成功。
using ReloadFn = std::function<bool(int64_t ver, std::string& msg)>;

void registerModelApi(httplib::Server& svr, ConfigDb& db, ReloadFn on_reload = nullptr);

} // namespace industrial
