// src/web_server.cpp — 内置 HTTP 管理界面
// httplib v0.12.6：不定义 CPPHTTPLIB_ZLIB/OPENSSL/BROTLI_SUPPORT 即可禁用这些功能
#include <httplib.h>
#include "web_server.h"
#include "config_db.h"
#include "config.h"

// 编译期注入的源码 web/ 目录（CMake 传入），debug 模式默认从此处实时伺服
#ifndef IC_WEB_SOURCE_DIR
#define IC_WEB_SOURCE_DIR ""
#endif
// web_assets_gen.h 由 CMake add_custom_command 从 web/index.html 生成
#include "web_assets_gen.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;
namespace industrial {

// ── 鉴权辅助 ─────────────────────────────────────────────────────────────────
// 定时安全比较：避免按字节提前返回而泄漏"已匹配前缀长度"。长度差异仍会泄漏，
// 但对固定长度的 base64 凭据无实际意义。
static bool secureEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

static bool isLoopbackBind(const std::string& b) {
    return b == "127.0.0.1" || b == "::1" || b == "localhost";
}

// ── 构造 / 析构 ───────────────────────────────────────────────────────────────
WebServer::WebServer(SharedState& state, const WebConfig& wcfg, ConfigDb* cfgdb,
                     std::function<bool(int64_t, std::string&)> on_reload)
    : state_(state), wcfg_(wcfg), cfgdb_(cfgdb), on_reload_(std::move(on_reload)),
      port_(wcfg.port), bind_(wcfg.bind),
      debug_(wcfg.debug),
      debugRoot_(!wcfg.debug_root.empty() ? wcfg.debug_root
                                          : std::string(IC_WEB_SOURCE_DIR))
{
    authOn_ = wcfg.auth_enabled;
    if (authOn_) {
        // httplib 客户端侧同样用 "Basic " + base64(user:pass) 构造该头（见 httplib.h），
        // 故直接比较编码后的字符串即可，无需解码。httplib 版本锁定 v0.12.6。
        authHeader_ = "Basic " + httplib::detail::base64_encode(
                                     wcfg.auth_user + ":" + wcfg.auth_password);
    }
    if (wcfg.tls_enabled) {
        // HTTPS：用 SSLServer（is-a Server），证书无效则 listen 会静默失败，必须查
        auto ssl = std::make_unique<httplib::SSLServer>(
                       wcfg.tls_cert.c_str(), wcfg.tls_key.c_str());
        if (ssl->is_valid()) {
            tlsActive_ = true;
            svr_ = std::move(ssl);
            spdlog::info("Web HTTPS 已启用 (cert={}, key={})",
                         wcfg.tls_cert, wcfg.tls_key);
        } else {
            spdlog::error("HTTPS 证书/私钥无效 (cert={}, key={})，回退明文 HTTP；"
                          "可运行 scripts/gen_cert.sh 生成自签证书",
                          wcfg.tls_cert, wcfg.tls_key);
            svr_ = std::make_unique<httplib::Server>();
        }
    } else {
        svr_ = std::make_unique<httplib::Server>();
    }
}

WebServer::~WebServer() { stop(); }

void WebServer::start() {
    // 失败关闭：声明要鉴权却没配密码，宁可不启动 Web，也不能开一个不设防的管理面
    if (authOn_ && wcfg_.auth_password.empty()) {
        spdlog::critical("web.auth_enabled=true 但 web.auth_password 为空 —— "
                         "Web 管理界面拒绝启动（避免暴露无鉴权的停机/改配置接口）");
        return;
    }
    if (authOn_ && !tlsActive_ && !isLoopbackBind(bind_))
        spdlog::warn("Web 鉴权已开但未启用 TLS：Basic 凭据在网络上等同明文传输，"
                     "公网暴露请同时设置 web.tls_enabled=true");
    if (!authOn_ && !isLoopbackBind(bind_))
        spdlog::warn("⚠ 安全警告：Web 管理界面监听 {} 且未启用鉴权 —— 任何可访问该端口的人"
                     "都能 POST /api/control 停机、POST /api/config/full 改写配置。"
                     "请设置 web.auth_enabled=true（并配 auth_user/auth_password），"
                     "或将 web.bind 改回 127.0.0.1", bind_);

    setupRoutes();
    const char* scheme = tlsActive_ ? "https" : "http";
    thread_ = std::thread([this, scheme] {
        spdlog::info("Web 管理界面: {}://{}:{}", scheme, bind_, port_);
        if (!svr_->listen(bind_.c_str(), port_))
            spdlog::error("Web 服务启动失败：{}:{} 无法监听（端口被占用或权限不足？）",
                          bind_, port_);
    });
}

void WebServer::stop() {
    svr_->stop();
    if (thread_.joinable()) thread_.join();
}

// ── 路由注册 ─────────────────────────────────────────────────────────────────
void WebServer::setupRoutes() {
    // 鉴权闸门：拦在所有路由之前，静态页与 /api/* 一视同仁。
    // 只保护写接口是不够的 —— /api/logs、/api/points 同样会泄漏现场数据。
    if (authOn_) {
        svr_->set_pre_routing_handler(
            [this](const httplib::Request& req, httplib::Response& res) {
                auto it = req.headers.find("Authorization");
                if (it != req.headers.end() && secureEquals(it->second, authHeader_))
                    return httplib::Server::HandlerResponse::Unhandled;   // 放行到正常路由
                res.status = 401;
                res.set_header("WWW-Authenticate",
                               "Basic realm=\"industrial_collector\", charset=\"UTF-8\"");
                res.set_content(R"({"ok":false,"error":"unauthorized"})",
                                "application/json");
                return httplib::Server::HandlerResponse::Handled;
            });
    }

    // keep-alive 超时设为 1s：比前端 2s 轮询间隔短，确保每次 refresh 建新连接，
    // 避免浏览器复用已关闭的 stale 连接导致 ERR_CONNECTION_TIMED_OUT
    // 【只设 SO_REUSEADDR，不设 SO_REUSEPORT】
    // httplib 默认两个都设。SO_REUSEPORT 会让第二个实例静默绑定成功，内核在
    // 两者之间轮流分发请求 —— 误启两份时不报任何错，症状却是"数据时有时无、
    // 配置改了只有一半生效"（请求随机落到新旧两个进程），极难排查。
    // 只留 SO_REUSEADDR：仍可在 TIME_WAIT 残留时立即重启，但重复启动会如常
    // 报 "Address already in use" 而立刻失败。
    // socket_t 定义在全局作用域（httplib.h 的平台头之后），不在 httplib 命名空间内
    svr_->set_socket_options([](socket_t sock) {
        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const void*>(&yes), sizeof(yes));
    });

    svr_->set_keep_alive_max_count(10);
    svr_->set_keep_alive_timeout(1);
    svr_->set_read_timeout(5, 0);
    svr_->set_write_timeout(5, 0);

    svr_->set_default_headers({
        {"Access-Control-Allow-Origin",  "*"},
        {"Access-Control-Allow-Methods", "GET,POST,PUT,OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"},
        {"Cache-Control",                "no-cache"}
    });
    svr_->Options(".*", [](const httplib::Request&, httplib::Response& res){
        res.status = 204;
    });

    // 静态资源：debug 模式从磁盘 web/ 目录实时伺服（改完刷新即可）；
    // 否则用编译时打包进 web_assets_gen.h 的字节数组。
    bool mounted = false;
    if (debug_ && !debugRoot_.empty()) {
        mounted = svr_->set_mount_point("/", debugRoot_);
        if (mounted)
            spdlog::warn("【DEBUG】前端从磁盘实时伺服: {}（改完刷新即可，无需重编）",
                         debugRoot_);
        else
            spdlog::error("【DEBUG】挂载 {} 失败，回退到编译内嵌前端", debugRoot_);
    }
    if (!mounted) {
        // 编译内嵌：从生成的资源表注册各路由
        for (size_t i = 0; i < WEB_ASSET_COUNT; ++i) {
            const auto& a = WEB_ASSETS[i];
            std::string body(a.data, a.size);
            std::string mime = a.mime;
            svr_->Get(a.path, [body, mime](const httplib::Request&, httplib::Response& res){
                res.set_content(body, mime.c_str());
            });
        }
    }

    svr_->Get("/api/all",             [this](auto& q, auto& r){ handleApiAll(q, r); });
    svr_->Get("/api/stream",          [this](auto& q, auto& r){ handleApiStream(q, r); });
    svr_->Get("/api/status",          [this](auto& q, auto& r){ handleApiStatus(q, r); });
    svr_->Get("/api/points",          [this](auto& q, auto& r){ handleApiPoints(q, r); });
    svr_->Get("/api/tree/live",       [this](auto& q, auto& r){ handleApiTreeLive(q, r); });
    svr_->Post("/api/channels/reload", [this](const httplib::Request&, httplib::Response& r) {
        // 通道表改完热重建；不带任何请求体。返回 {ok, msg}。
        if (!on_ch_reload_) {
            r.status = 501;
            r.set_content(R"({"ok":false,"error":"本进程未接入通道热切换"})", "application/json");
            return;
        }
        std::string msg;
        const bool ok = on_ch_reload_(msg);
        nlohmann::json j = {{"ok", ok}, {"msg", msg}};
        r.set_content(j.dump(), "application/json");
    });
    svr_->Get("/api/logs",            [this](auto& q, auto& r){ handleApiLogs(q, r); });
    svr_->Get("/favicon.ico",         [](const httplib::Request&, httplib::Response& r){ r.status = 204; });
    svr_->Get("/api/config",          [this](auto& q, auto& r){ handleApiConfig(q, r); });
    svr_->Put("/api/config",          [this](auto& q, auto& r){ handleApiConfigPut(q, r); });
    svr_->Get("/api/config/full",     [this](auto& q, auto& r){ handleApiConfigFullGet(q, r); });
    svr_->Post("/api/config/full",    [this](auto& q, auto& r){ handleApiConfigFullPost(q, r); });
    svr_->Post("/api/control",        [this](auto& q, auto& r){ handleApiControl(q, r); });

    // 模型驱动板块（设备规格 / 设备树 / 任务 / 通道 / 设置）。
    // 鉴权已由上面的 set_pre_routing_handler 统一覆盖，这些路由无需各自校验。
    if (cfgdb_) registerModelApi(*svr_, *cfgdb_, on_reload_);
}

// ── buildAllJson：status + points + logs，供 /api/all 和 SSE 共用 ─────────────
std::string WebServer::buildAllJson(int n) {
    json j;
    {
        json& s = j["status"];
        s["running"]        = state_.running.load();
        s["paused"]         = state_.paused.load();
        s["total_polls"]    = state_.totalPolls();
        s["total_errors"]   = state_.totalErrors();
        s["total_bad_points"] = state_.totalBadPoints();
        s["mqtt_published"] = state_.totalPublished();
        std::lock_guard<std::mutex> lk(state_.stats_mutex);
        json devArr = json::object();
        for (auto& kv : state_.device_stats)
            devArr[kv.first] = {
                {"protocol",       kv.second.protocol},
                {"status",         kv.second.status},
                {"start_time",     kv.second.start_time},
                {"total_polls",    kv.second.total_polls},
                {"good_polls",     kv.second.good_polls},
                {"dead_polls",     kv.second.dead_polls},
                {"errors",         kv.second.errors},
                {"total_points",   kv.second.total_points},
                {"bad_points",     kv.second.bad_points},
                {"mqtt_published", kv.second.mqtt_published}};
        s["devices"] = devArr;
        if (!state_.config.mqtt.broker.empty())
            s["broker"] = state_.config.mqtt.broker + ":" +
                          std::to_string(state_.config.mqtt.port);
    }
    {
        // 每个设备只带前 SSE_PAGE_CAP 个点：储能站一台 13084 点，全塞进 SSE
        // 每帧 ~2MB × 3s = 5Mbps 白白烧掉，浏览器一次渲染 13084 格 DOM 直接
        // 卡半天。前 N 个够小设备用；大设备翻页由前端调 /api/points 拉。
        // 形状统一成 {total, list}，前端一处判断；小设备也带 total，方便页脚
        // 显示 "3 点"。
        constexpr size_t SSE_PAGE_CAP = 200;
        json result = json::object();
        std::lock_guard<std::mutex> lk(state_.points_mutex);
        for (auto& kv : state_.device_points) {
            json list = json::array();
            const size_t n = std::min(kv.second.size(), SSE_PAGE_CAP);
            for (size_t i = 0; i < n; ++i) {
                const auto& p = kv.second[i];
                json item;
                item["name"]        = p.name;
                item["quality"]     = qualityStr(p.quality);
                item["timestamp"]   = p.timestamp;
                item["unit"]        = p.unit;
                item["description"] = p.description;
                auto v = p.toDouble();
                item["value"] = v.has_value() ? json(*v) : json(nullptr);
                list.push_back(std::move(item));
            }
            result[kv.first] = { {"total", kv.second.size()}, {"list", std::move(list)} };
        }
        j["points"] = result;
    }
    {
        std::deque<LogEntry> logs;
        std::lock_guard<std::mutex> lk(state_.log_mutex);
        auto& ring = state_.log_ring;
        auto  beg  = ring.size() > size_t(n) ? ring.end()-n : ring.begin();
        logs = std::deque<LogEntry>(beg, ring.end());
        j["logs"] = json::parse(logsToJson(logs));
    }
    return j.dump();
}

// ── /api/all ─────────────────────────────────────────────────────────────────
void WebServer::handleApiAll(const httplib::Request& req, httplib::Response& res) {
    int n = 80;
    if (req.has_param("n")) try { n = std::stoi(req.get_param_value("n")); } catch(...) {}
    res.set_content(buildAllJson(std::min(n, 500)), "application/json");
}

// ── /api/stream  Server-Sent Events ──────────────────────────────────────────
void WebServer::handleApiStream(const httplib::Request&, httplib::Response& res) {
    res.set_header("Content-Type",      "text/event-stream");
    res.set_header("Cache-Control",     "no-cache");
    res.set_header("X-Accel-Buffering", "no");   // 禁用 nginx 缓冲

    res.set_chunked_content_provider("text/event-stream",
        [this, first = true](size_t /*offset*/, httplib::DataSink& sink) mutable -> bool {
            if (first) {
                // 连接建立后立即推一次当前数据，无需等待下次设备更新
                first = false;
            } else {
                // 等待设备数据更新或 3 秒心跳超时
                uint64_t seq = state_.push_seq_.load(std::memory_order_relaxed);
                std::unique_lock<std::mutex> lk(state_.push_mtx_);
                bool updated = state_.push_cv_.wait_for(lk,
                    std::chrono::seconds(3),
                    [&]{ return state_.push_seq_.load(std::memory_order_relaxed) != seq; });
                lk.unlock();

                if (!updated) {
                    // 心跳注释行，防止代理/浏览器认为连接断开
                    static const std::string hb = ": hb\n\n";
                    return sink.write(hb.data(), hb.size());
                    // sink.write 返回 false 即表示连接断开，调用方会停止回调
                }
            }

            std::string body = "data: " + buildAllJson(40) + "\n\n";
            return sink.write(body.data(), body.size());
        }
    );
}

// ── /api/status ──────────────────────────────────────────────────────────────
void WebServer::handleApiStatus(const httplib::Request&, httplib::Response& res) {
    json j;
    j["running"] = state_.running.load();
    j["paused"]  = state_.paused.load();
    j["total_polls"]    = state_.totalPolls();
    j["total_errors"]   = state_.totalErrors();
    j["total_bad_points"] = state_.totalBadPoints();
    j["mqtt_published"] = state_.totalPublished();
    {
        std::lock_guard<std::mutex> lk(state_.stats_mutex);
        json devArr = json::object();
        for (auto& kv : state_.device_stats) {
            devArr[kv.first] = {
                {"protocol",       kv.second.protocol},
                {"status",         kv.second.status},
                {"start_time",     kv.second.start_time},
                {"total_polls",    kv.second.total_polls},
                {"good_polls",     kv.second.good_polls},
                {"dead_polls",     kv.second.dead_polls},
                {"errors",         kv.second.errors},
                {"total_points",   kv.second.total_points},
                {"bad_points",     kv.second.bad_points},
                {"mqtt_published", kv.second.mqtt_published}
            };
        }
        j["devices"] = devArr;
    }
    {
        std::lock_guard<std::mutex> lk(state_.config_mutex);
        if (!state_.config.mqtt.broker.empty())
            j["broker"] = state_.config.mqtt.broker + ":" +
                          std::to_string(state_.config.mqtt.port);
    }
    res.set_content(j.dump(), "application/json");
}

// ── /api/points ──────────────────────────────────────────────────────────────
//
// 两种用法：
//   1. 传 device + page + size  → 返回单设备指定页：{total,page,size,list:[...]}
//   2. 不传 device            → 返回所有设备各自的 {total,list} —— 每设备只带前
//                                SSE_PAGE_CAP 条，与 /api/all 的形状对齐；给
//                                "全部设备" 视图当兜底数据源用
//
// 老形状 `{device:[points]}` 不再兼容 —— 前端是唯一消费者，一起改。
void WebServer::handleApiPoints(const httplib::Request& req, httplib::Response& res) {
    const std::string dev = req.has_param("device") ? req.get_param_value("device")
                                                     : (req.has_param("id") ? req.get_param_value("id") : "");
    int page = 1, size = 200;
    if (req.has_param("page")) try { page = std::stoi(req.get_param_value("page")); } catch(...) {}
    if (req.has_param("size")) try { size = std::stoi(req.get_param_value("size")); } catch(...) {}
    if (page < 1)   page = 1;
    if (size < 1)   size = 1;
    if (size > 1000) size = 1000;   // 单页硬上限，前端做错也不至于把响应打爆

    json out = json::object();
    std::lock_guard<std::mutex> lk(state_.points_mutex);

    if (!dev.empty()) {
        auto it = state_.device_points.find(dev);
        const size_t total = (it == state_.device_points.end()) ? 0 : it->second.size();
        const size_t begin = std::min<size_t>((size_t)(page - 1) * size, total);
        const size_t end   = std::min<size_t>(begin + size, total);
        json list = json::array();
        if (it != state_.device_points.end()) {
            for (size_t i = begin; i < end; ++i) {
                const auto& p = it->second[i];
                json item;
                item["name"]        = p.name;
                item["quality"]     = qualityStr(p.quality);
                item["timestamp"]   = p.timestamp;
                item["unit"]        = p.unit;
                item["description"] = p.description;
                auto v = p.toDouble();
                item["value"] = v.has_value() ? json(*v) : json(nullptr);
                list.push_back(std::move(item));
            }
        }
        out["device"] = dev;
        out["total"]  = total;
        out["page"]   = page;
        out["size"]   = size;
        out["list"]   = std::move(list);
    } else {
        // 兜底：与 SSE 一样每设备只带前 200 条。全局遍历还全量下发的话，
        // 打开一次监控页 = 4MB 响应，无谓。
        constexpr size_t CAP = 200;
        for (auto& kv : state_.device_points) {
            json list = json::array();
            const size_t n = std::min(kv.second.size(), CAP);
            for (size_t i = 0; i < n; ++i) {
                const auto& p = kv.second[i];
                json item;
                item["name"]        = p.name;
                item["quality"]     = qualityStr(p.quality);
                item["timestamp"]   = p.timestamp;
                item["unit"]        = p.unit;
                item["description"] = p.description;
                auto v = p.toDouble();
                item["value"] = v.has_value() ? json(*v) : json(nullptr);
                list.push_back(std::move(item));
            }
            out[kv.first] = { {"total", kv.second.size()}, {"list", std::move(list)} };
        }
    }
    res.set_content(out.dump(), "application/json");
}

// ── /api/tree/live ───────────────────────────────────────────────────────────
//
// 树形下钻的实时数据接口。取一个节点路径，返回：
//   - 该节点的直接子节点（结构 + 每个子节点的自有点位快照）
//   - 该节点自己挂着的点位（包节点的 pack_voltage、cell_v_max 之类）
//
// 判据是【层级自洽】：包节点的 pack_voltage 与它 24 个子节点电压之和差应
// 落在 scale 精度以内 —— 这正是 ess_modbus_sim.py 按拓扑造数的原因，前端
// 拿这两个值并列展示就能一眼看出聚合有没有算错，比"整齐、漂亮"重要得多。
//
// 【为什么不接受 task 参数】：设备树是任务无关的（一棵树可以被多个任务覆盖，
// 尽管当前 ESS 只有 bms_modbus 一个）。全 task 遍历 state.device_points 找
// 匹配的名字即可。13084 点的全量扫每次 <1ms，值不到为它做二级索引。
void WebServer::handleApiTreeLive(const httplib::Request& req, httplib::Response& res) {
    if (!cfgdb_ || !cfgdb_->ok()) {
        res.status = 503;
        res.set_content(R"({"ok":false,"error":"配置库不可用（未启动或建库失败）"})",
                        "application/json");
        return;
    }

    const std::string path = req.has_param("path") ? req.get_param_value("path") : "";

    // 定位节点：path="" 时返回根一层
    std::optional<int64_t> parentId;
    std::string parentName;
    if (!path.empty()) {
        auto n = cfgdb_->nodeByPath(path);
        if (!n) {
            res.status = 404;
            json err = { {"ok", false}, {"error", "节点不存在: " + path} };
            res.set_content(err.dump(), "application/json");
            return;
        }
        parentId   = n->id;
        // 实例化时若没显式给名字（instantiateModel 就是这样，为的是节点数上万时
        // 不硬塞冗余名字），name 是空的 —— 回退到 code，别用误导性的 "(根)"
        parentName = !n->name.empty() ? n->name : n->code;
    }

    // 一次性把当前实时数据抓下来 —— 拉住锁的时间越短越好，然后按前缀过滤
    std::unordered_map<std::string, DataPoints> snapshot;
    {
        std::lock_guard<std::mutex> lk(state_.points_mutex);
        snapshot = state_.device_points;
    }

    // 建"全路径 name → DataPoint" 索引一次，之后 O(1) 查
    // 全路径 = 采集器的 name 字段本身（"station.rackXX...pack1/pack_voltage"）
    std::unordered_map<std::string, const DataPoint*> byName;
    for (const auto& kv : snapshot)
        for (const auto& p : kv.second) byName[p.name] = &p;

    auto ptToJson = [](const DataPoint& p) {
        json j;
        j["name"]      = p.name;
        j["quality"]   = qualityStr(p.quality);
        j["timestamp"] = p.timestamp;
        j["unit"]      = p.unit;
        auto v = p.toDouble();
        j["value"] = v.has_value() ? json(*v) : json(nullptr);
        return j;
    };

    // 收集"以 <prefix>/ 开头且 '/' 之后无 '.'" 的点 —— 那是本节点直挂的
    auto ownPointsAt = [&](const std::string& nodePath) {
        json arr = json::array();
        const std::string pfx = nodePath + "/";
        for (const auto& kv : byName) {
            const std::string& n = kv.first;
            if (n.rfind(pfx, 0) != 0) continue;
            // '/' 之后再有 '.' 说明这属于更深的后代（例：pack1.cell01/voltage
            // 不属于 pack1，属于 cell01）
            if (n.find('.', pfx.size()) != std::string::npos) continue;
            arr.push_back(ptToJson(*kv.second));
        }
        return arr;
    };

    json out;
    out["path"]  = path;
    out["name"]  = parentName.empty() ? std::string("(根)") : parentName;
    out["own_points"] = ownPointsAt(path);

    // 直接子节点：结构从 DB 拿（哪怕它没点位），值从 snapshot 拿
    json kids = json::array();
    for (const auto& [row, cc] : cfgdb_->listChildren(parentId)) {
        json c;
        c["path"]        = row.path;
        c["name"]        = row.name.empty() ? row.code : row.name;
        c["code"]        = row.code;
        c["model"]       = row.model ? *row.model : "";
        c["child_count"] = cc;
        c["points"]      = ownPointsAt(row.path);
        kids.push_back(std::move(c));
    }
    out["children"] = std::move(kids);

    res.set_content(out.dump(), "application/json");
}

// ── /api/logs ────────────────────────────────────────────────────────────────
void WebServer::handleApiLogs(const httplib::Request& req, httplib::Response& res) {
    int n = 100;
    if (req.has_param("n")) try { n = std::stoi(req.get_param_value("n")); } catch(...) {}
    n = std::min(n, 500);
    std::deque<LogEntry> logs;
    {
        std::lock_guard<std::mutex> lk(state_.log_mutex);
        auto& ring = state_.log_ring;
        auto  beg  = ring.size() > size_t(n) ? ring.end()-n : ring.begin();
        logs = std::deque<LogEntry>(beg, ring.end());
    }
    res.set_content(logsToJson(logs), "application/json");
}

// ── /api/config GET ───────────────────────────────────────────────────────────
void WebServer::handleApiConfig(const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lk(state_.config_mutex);
    auto& cfg = state_.config;
    json j;
    j["mqtt"]["broker"]       = cfg.mqtt.broker;
    j["mqtt"]["port"]         = cfg.mqtt.port;
    j["mqtt"]["topic_prefix"] = cfg.mqtt.topic_prefix;
    j["mqtt"]["qos"]          = cfg.mqtt.qos;
    j["mqtt"]["keepalive"]    = cfg.mqtt.keepalive;
    j["web"]["port"]    = cfg.web.port;
    j["web"]["enabled"] = cfg.web.enabled;

    json devArr = json::array();
    for (auto& e : cfg.devices) {
        json d;
        d["id"]       = e.id;
        d["enabled"]  = e.enabled;
        d["protocol"] = protocolStr(e.protocol);
        d["poll_interval"] = e.pollInterval();
        switch (e.protocol) {
            case Protocol::MODBUS:
                d["endpoint"] = e.modbus.host+":"+std::to_string(e.modbus.port);
                d["point_count"] = (int)e.modbus.points.size();
                break;
            case Protocol::IEC104:
                d["endpoint"] = e.iec104.host+":"+std::to_string(e.iec104.port);
                d["point_count"] = (int)e.iec104.points.size();
                break;
            case Protocol::IEC61850:
                d["endpoint"] = e.iec61850.host+":"+std::to_string(e.iec61850.port);
                d["point_count"] = (int)e.iec61850.points.size();
                break;
            case Protocol::OPCUA:
                d["endpoint"] = e.opcua.endpoint_url;
                d["point_count"] = (int)e.opcua.points.size();
                break;
            case Protocol::DLT645:
                d["endpoint"] = e.dlt645.connection_type=="tcp"
                    ? e.dlt645.host+":"+std::to_string(e.dlt645.tcp_port)
                    : e.dlt645.serial_port;
                d["meter_count"] = (int)e.dlt645.meters.size();
                break;
            case Protocol::DLT698:
                d["endpoint"] = e.dlt698.connection_type=="tcp"
                    ? e.dlt698.host+":"+std::to_string(e.dlt698.tcp_port)
                    : e.dlt698.serial_port;
                d["meter_count"] = (int)e.dlt698.meters.size();
                break;
            case Protocol::CAN:
                d["endpoint"] = e.can.interface + (e.can.fd ? " (FD)" : "");
                d["point_count"] = (int)e.can.signals.size();
                break;
        }
        devArr.push_back(std::move(d));
    }
    j["devices"] = devArr;
    res.set_content(j.dump(2), "application/json");
}

// ── /api/config PUT（热更新：MQTT参数）──────────────────────────────────────
void WebServer::handleApiConfigPut(const httplib::Request& req, httplib::Response& res) {
    try {
        auto j = json::parse(req.body);
        std::lock_guard<std::mutex> lk(state_.config_mutex);
        if (j.contains("topic_prefix"))
            state_.config.mqtt.topic_prefix = j["topic_prefix"].get<std::string>();
        if (j.contains("qos"))
            state_.config.mqtt.qos = j["qos"].get<int>();
        if (j.contains("poll_interval") && j.contains("device_id")) {
            double iv = j["poll_interval"].get<double>();
            std::string did = j["device_id"].get<std::string>();
            for (auto& e : state_.config.devices) {
                if (e.id != did) continue;
                switch(e.protocol) {
                    case Protocol::MODBUS:   e.modbus.poll_interval   = iv; break;
                    case Protocol::IEC104:   e.iec104.poll_interval   = iv; break;
                    case Protocol::IEC61850: e.iec61850.poll_interval = iv; break;
                    case Protocol::OPCUA:    e.opcua.poll_interval    = iv; break;
                    case Protocol::DLT645:   e.dlt645.poll_interval   = iv; break;
                    case Protocol::DLT698:   e.dlt698.poll_interval   = iv; break;
                    case Protocol::CAN:      e.can.poll_interval      = iv; break;
                }
                break;
            }
        }
        if (!state_.config_path.empty()) {
            try { saveConfig(state_.config, state_.config_path); } catch (...) {}
        }
        state_.addLog("INFO", "配置已热更新");
        res.set_content(R"({"ok":true})", "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        json err; err["ok"]=false; err["error"]=e.what();
        res.set_content(err.dump(), "application/json");
    }
}

// ── /api/control ─────────────────────────────────────────────────────────────
void WebServer::handleApiControl(const httplib::Request& req, httplib::Response& res) {
    try {
        auto j = json::parse(req.body);
        std::string action = j.value("action","");
        if      (action=="pause")  { state_.paused=true;  state_.addLog("WARN","采集已暂停"); }
        else if (action=="resume") { state_.paused=false; state_.addLog("INFO","采集已恢复"); }
        else if (action=="stop")   { state_.running=false;state_.addLog("WARN","程序停止指令"); }
        else {
            res.status=400;
            res.set_content(R"({"ok":false,"error":"unknown action"})","application/json");
            return;
        }
        res.set_content(R"({"ok":true})","application/json");
    } catch (const std::exception& e) {
        res.status=400;
        json err; err["ok"]=false; err["error"]=e.what();
        res.set_content(err.dump(),"application/json");
    }
}

// ── JSON 序列化 ───────────────────────────────────────────────────────────────
std::string WebServer::pointsToJson(const DataPoints& pts) {
    json arr = json::array();
    for (const auto& p : pts) {
        json item;
        item["name"]        = p.name;
        item["quality"]     = qualityStr(p.quality);
        item["timestamp"]   = p.timestamp;
        item["unit"]        = p.unit;
        item["description"] = p.description;
        auto v = p.toDouble();
        item["value"] = v.has_value() ? json(*v) : json(nullptr);
        arr.push_back(item);
    }
    return arr.dump();
}

std::string WebServer::logsToJson(const std::deque<LogEntry>& logs) {
    json arr = json::array();
    for (const auto& e : logs)
        arr.push_back({{"ts", e.timestamp}, {"level", e.level}, {"msg", e.message}});
    return arr.dump();
}

// ── /api/config/full GET ─────────────────────────────────────────────────────
// 配置里的密码不回显给浏览器：GET 时替换成哨兵，POST 时若字段仍是哨兵就沿用原值。
// 用户改密码 → 前端发新明文；用户清空 → 发空串（真的清空）；用户没动 → 发哨兵。
// 哨兵取一个不会被当作真实密码的串；万一用户真把密码设成它，也只是"改不了密码"，
// 不会泄漏或丢失原值。
static const char* SECRET_MASK = "********";

static void maskSecrets(json& j) {
    auto mask = [](json& node, const char* key) {
        if (node.contains(key) && node[key].is_string() &&
            !node[key].get<std::string>().empty())
            node[key] = SECRET_MASK;
    };
    if (j.contains("mqtt")) mask(j["mqtt"], "password");
    if (j.contains("web"))  mask(j["web"],  "auth_password");
    if (j.contains("devices"))
        for (auto& d : j["devices"]) {
            if (d.contains("opcua"))    mask(d["opcua"],    "password");
            if (d.contains("iec61850")) mask(d["iec61850"], "auth_password");
        }
}

// 把仍是哨兵的密码换回当前配置里的真实值（按设备 id 对应）
static void restoreSecrets(AppConfig& neu, const AppConfig& cur) {
    if (neu.mqtt.password      == SECRET_MASK) neu.mqtt.password      = cur.mqtt.password;
    if (neu.web.auth_password  == SECRET_MASK) neu.web.auth_password  = cur.web.auth_password;
    for (auto& d : neu.devices) {
        const DeviceEntry* old = nullptr;
        for (const auto& o : cur.devices)
            if (o.id == d.id) { old = &o; break; }
        // 新增设备、或设备被改了 id：找不到旧值。此时宁可清空，也不能把哨兵
        // 当成真密码存进去（那会是个谁也不知道的"密码 ********"）。
        const std::string fallback;
        const std::string& oldOpcua = old ? old->opcua.password         : fallback;
        const std::string& oldIec   = old ? old->iec61850.auth_password : fallback;
        if (d.opcua.password         == SECRET_MASK) d.opcua.password         = oldOpcua;
        if (d.iec61850.auth_password == SECRET_MASK) d.iec61850.auth_password = oldIec;
    }
}

void WebServer::handleApiConfigFullGet(const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lk(state_.config_mutex);
    json j = json::parse(appConfigToJsonString(state_.config));
    maskSecrets(j);
    res.set_content(j.dump(2), "application/json");
}

// ── /api/config/full POST ────────────────────────────────────────────────────
void WebServer::handleApiConfigFullPost(const httplib::Request& req, httplib::Response& res) {
    try {
        AppConfig newCfg = appConfigFromJsonString(req.body);
        // 不允许通过 API 关闭 web（会把自己切断）
        newCfg.web.enabled = true;

        std::string path;
        {
            std::lock_guard<std::mutex> lk(state_.config_mutex);
            // GET 时密码被替换成哨兵；未改动的字段要换回真实值，否则保存会把密码抹掉
            restoreSecrets(newCfg, state_.config);
            path = state_.config_path;
            state_.config = newCfg;
        }
        if (!path.empty()) saveConfig(newCfg, path);

        state_.addLog("INFO", "配置已通过 Web API 保存");
        json resp;
        resp["ok"] = true;
        resp["message"] = "配置已保存；MQTT/缓存参数立即生效，设备连接参数重启后生效";
        resp["live"]    = json::array({"mqtt","cache","logging"});
        resp["restart"] = json::array({"devices"});
        res.set_content(resp.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        json err; err["ok"] = false; err["error"] = e.what();
        res.set_content(err.dump(), "application/json");
    }
}

} // namespace industrial
