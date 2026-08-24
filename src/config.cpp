// src/config.cpp — 多设备混合协议配置解析
#include "config.h"
#include <fstream>
#include <stdexcept>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace industrial {

#define JG(j,k,d) ((j).contains(k) ? (j).at(k).get<decltype(d)>() : (d))

// ── 辅助 ─────────────────────────────────────────────────────────────────────
static ModbusDataType parseDataType(const std::string& s) {
    if (s=="bool")                return ModbusDataType::BOOL;
    if (s=="int16")               return ModbusDataType::INT16;
    if (s=="uint16")              return ModbusDataType::UINT16;
    if (s=="int32")               return ModbusDataType::INT32;
    if (s=="uint32")              return ModbusDataType::UINT32;
    if (s=="float"||s=="float32") return ModbusDataType::FLOAT32;
    throw std::runtime_error("未知 data_type: " + s);
}
static std::string dataTypeStr(ModbusDataType t) {
    switch(t){
        case ModbusDataType::BOOL:    return "bool";
        case ModbusDataType::INT16:   return "int16";
        case ModbusDataType::UINT16:  return "uint16";
        case ModbusDataType::INT32:   return "int32";
        case ModbusDataType::UINT32:  return "uint32";
        case ModbusDataType::FLOAT32: return "float";
    }
    return "uint16";
}

// 去首尾空白并转小写，容忍 "CAN" / "canfd " 这类写法。
// 规范化后的串同时用于 parseProtocol 和 "canfd" 简写判定，二者必须看同一个值。
static std::string normProtocol(std::string s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    s = s.substr(b, s.find_last_not_of(ws) - b + 1);
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// 注意：不得对未知协议名回退到 MODBUS —— 那会让 "CANN" 这类笔误静默变成一个
// 去连 127.0.0.1:502 的 Modbus 设备，排查代价极高。宁可启动失败。
// s = 规范化后的值（用于匹配），raw = 用户原样写的值（用于报错，便于在文件里搜到）。
static Protocol parseProtocol(const std::string& s, const std::string& raw,
                              const std::string& devId) {
    if (s=="modbus")   return Protocol::MODBUS;
    if (s=="iec104")   return Protocol::IEC104;
    if (s=="iec61850") return Protocol::IEC61850;
    if (s=="opcua")    return Protocol::OPCUA;
    if (s=="dlt645")   return Protocol::DLT645;
    if (s=="dlt698")   return Protocol::DLT698;
    // "canfd" 是 "can" + can.fd=true 的简写，共用同一采集器
    if (s=="can" || s=="canfd") return Protocol::CAN;
    throw std::runtime_error("设备 \"" + devId + "\" 的 protocol 未知: \"" + raw +
        "\"；可选: modbus / iec104 / iec61850 / opcua / dlt645 / dlt698 / can / canfd");
}
static const char* protoStr(Protocol p) {
    switch(p){
        case Protocol::MODBUS:   return "modbus";
        case Protocol::IEC104:   return "iec104";
        case Protocol::IEC61850: return "iec61850";
        case Protocol::OPCUA:    return "opcua";
        case Protocol::DLT645:   return "dlt645";
        case Protocol::DLT698:   return "dlt698";
        case Protocol::CAN:      return "can";
    }
    return "modbus";
}

// ── CAN 辅助 ──────────────────────────────────────────────────────────────────
static CanByteOrder parseByteOrder(const std::string& s) {
    if (s=="little" || s=="intel")    return CanByteOrder::LITTLE;
    if (s=="big"    || s=="motorola") return CanByteOrder::BIG;
    throw std::runtime_error("未知 byte_order: " + s);
}
static const char* byteOrderStr(CanByteOrder b) {
    return b == CanByteOrder::BIG ? "big" : "little";
}
// can_id 接受数字（291）或十六进制字符串（"0x123"）
static uint32_t parseCanId(const json& v) {
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        try { return (uint32_t)std::stoul(s, nullptr, 0); }
        catch (...) { throw std::runtime_error("无法解析 can_id: " + s); }
    }
    return v.get<uint32_t>();
}
static std::string canIdHex(uint32_t id) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%X", id);
    return b;
}

// ── 各协议点表解析 ────────────────────────────────────────────────────────────
static int regWidth(ModbusDataType dt) {
    return (dt == ModbusDataType::INT32 || dt == ModbusDataType::UINT32 ||
            dt == ModbusDataType::FLOAT32) ? 2 : 1;
}

static ModbusConfig parseModbus(const json& m) {
    ModbusConfig c;
    c.host          = JG(m,"host",         c.host);
    c.port          = JG(m,"port",         c.port);
    c.unit_id       = JG(m,"unit_id",      c.unit_id);
    c.timeout_sec   = JG(m,"timeout",      c.timeout_sec);
    c.poll_interval = JG(m,"poll_interval",c.poll_interval);
    c.publish_mode  = JG(m,"publish_mode", c.publish_mode);

    // ── 命名点（手配）────────────────────────────────────────────────────────
    if (m.contains("points"))
        for (auto& p : m["points"]) {
            ModbusPoint mp;
            mp.name        = p.at("name").get<std::string>();
            mp.address     = p.at("address").get<int>();
            mp.func_code   = JG(p,"func_code", 3);
            mp.data_type   = parseDataType(JG(p,"data_type",std::string("uint16")));
            mp.scale       = JG(p,"scale",  1.0f);
            mp.offset      = JG(p,"offset", 0.0f);
            mp.unit        = JG(p,"unit",        std::string(""));
            mp.description = JG(p,"description", std::string(""));
            c.points.push_back(std::move(mp));
        }

    // ── 范围展开（大规模场景）────────────────────────────────────────────────
    if (m.contains("ranges"))
        for (auto& r : m["ranges"]) {
            ModbusRange mr;
            mr.name_prefix = r.at("name_prefix").get<std::string>();
            mr.addr_start  = r.at("addr_start").get<int>();
            mr.count       = r.at("count").get<int>();
            mr.func_code   = JG(r,"func_code",  3);
            mr.data_type   = parseDataType(JG(r,"data_type",std::string("uint16")));
            mr.scale       = JG(r,"scale",  1.0f);
            mr.offset      = JG(r,"offset", 0.0f);
            mr.unit        = JG(r,"unit",   std::string(""));
            mr.description = JG(r,"description", std::string(""));
            c.ranges.push_back(mr);

            int step = regWidth(mr.data_type);
            for (int i = 0; i < mr.count; ++i) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%04d", i);
                ModbusPoint mp;
                mp.name        = mr.name_prefix + buf;
                mp.address     = mr.addr_start + i * step;
                mp.func_code   = mr.func_code;
                mp.data_type   = mr.data_type;
                mp.scale       = mr.scale;
                mp.offset      = mr.offset;
                mp.unit        = mr.unit;
                mp.description = mr.description;
                mp.from_range  = true;
                c.points.push_back(std::move(mp));
            }
        }
    return c;
}

static IEC104Config parseIEC104(const json& m) {
    IEC104Config c;
    c.host           = JG(m,"host",           c.host);
    c.port           = JG(m,"port",           c.port);
    c.common_address = JG(m,"common_address", c.common_address);
    c.poll_interval  = JG(m,"poll_interval",  c.poll_interval);
    c.timeout_sec    = JG(m,"timeout",        c.timeout_sec);
    if (m.contains("points"))
        for (auto& p : m["points"]) {
            IEC104Point ip;
            ip.name        = p.at("name").get<std::string>();
            ip.ioa         = p.at("ioa").get<int>();
            ip.type_id     = p.at("type_id").get<int>();
            ip.scale       = JG(p,"scale",  1.0f);
            ip.offset      = JG(p,"offset", 0.0f);
            ip.unit        = JG(p,"unit",        std::string(""));
            ip.description = JG(p,"description", std::string(""));
            c.points.push_back(std::move(ip));
        }
    return c;
}

static IEC61850Config parseIEC61850(const json& m) {
    IEC61850Config c;
    c.host          = JG(m,"host",          c.host);
    c.port          = JG(m,"port",          c.port);
    c.poll_interval = JG(m,"poll_interval", c.poll_interval);
    c.timeout_ms    = JG(m,"timeout_ms",    c.timeout_ms);
    c.auth_password = JG(m,"auth_password", c.auth_password);
    c.use_reports   = JG(m,"use_reports",   c.use_reports);
    if (m.contains("points"))
        for (auto& p : m["points"]) {
            IEC61850Point ip;
            ip.name        = p.at("name").get<std::string>();
            ip.object_ref  = p.at("object_ref").get<std::string>();
            ip.fc          = JG(p,"fc", std::string("MX"));
            ip.scale       = JG(p,"scale",  1.0f);
            ip.offset      = JG(p,"offset", 0.0f);
            ip.unit        = JG(p,"unit",        std::string(""));
            ip.description = JG(p,"description", std::string(""));
            c.points.push_back(std::move(ip));
        }
    return c;
}

static OpcUaConfig parseOpcUa(const json& m) {
    OpcUaConfig c;
    c.endpoint_url   = JG(m,"endpoint_url",   c.endpoint_url);
    c.poll_interval  = JG(m,"poll_interval",  c.poll_interval);
    c.timeout_ms     = JG(m,"timeout_ms",     c.timeout_ms);
    c.security_policy= JG(m,"security_policy",c.security_policy);
    c.username       = JG(m,"username",       c.username);
    c.password       = JG(m,"password",       c.password);
    if (m.contains("points"))
        for (auto& p : m["points"]) {
            OpcUaPoint op;
            op.name        = p.at("name").get<std::string>();
            op.node_id     = p.at("node_id").get<std::string>();
            op.scale       = JG(p,"scale",  1.0f);
            op.offset      = JG(p,"offset", 0.0f);
            op.unit        = JG(p,"unit",        std::string(""));
            op.description = JG(p,"description", std::string(""));
            c.points.push_back(std::move(op));
        }
    return c;
}

static BusDlt645Config parseDlt645(const json& m) {
    BusDlt645Config c;
    c.connection_type = JG(m,"connection_type", c.connection_type);
    c.serial_port     = JG(m,"serial_port",     c.serial_port);
    c.baud_rate       = JG(m,"baud_rate",        c.baud_rate);
    c.host            = JG(m,"host",             c.host);
    c.tcp_port        = JG(m,"tcp_port",         c.tcp_port);
    c.poll_interval   = JG(m,"poll_interval",    c.poll_interval);
    c.timeout_ms      = JG(m,"timeout_ms",       c.timeout_ms);
    if (m.contains("meters"))
        for (auto& mt : m["meters"]) {
            MeterConfig645 mc;
            mc.id             = mt.at("id").get<std::string>();
            mc.meter_address  = JG(mt,"meter_address", std::string("000000000001"));
            if (mt.contains("points"))
                for (auto& p : mt["points"]) {
                    Dlt645Point dp;
                    dp.name        = p.at("name").get<std::string>();
                    dp.data_id     = p.at("data_id").get<std::string>();
                    dp.scale       = JG(p,"scale",  1.0f);
                    dp.offset      = JG(p,"offset", 0.0f);
                    dp.unit        = JG(p,"unit",        std::string(""));
                    dp.description = JG(p,"description", std::string(""));
                    mc.points.push_back(std::move(dp));
                }
            c.meters.push_back(std::move(mc));
        }
    return c;
}

static BusDlt698Config parseDlt698(const json& m) {
    BusDlt698Config c;
    c.connection_type = JG(m,"connection_type", c.connection_type);
    c.serial_port     = JG(m,"serial_port",     c.serial_port);
    c.baud_rate       = JG(m,"baud_rate",        c.baud_rate);
    c.host            = JG(m,"host",             c.host);
    c.tcp_port        = JG(m,"tcp_port",         c.tcp_port);
    c.poll_interval   = JG(m,"poll_interval",    c.poll_interval);
    c.timeout_ms      = JG(m,"timeout_ms",       c.timeout_ms);
    if (m.contains("meters"))
        for (auto& mt : m["meters"]) {
            MeterConfig698 mc;
            mc.id             = mt.at("id").get<std::string>();
            mc.meter_address  = JG(mt,"meter_address", std::string("000000000001"));
            if (mt.contains("points"))
                for (auto& p : mt["points"]) {
                    Dlt698Point dp;
                    dp.name        = p.at("name").get<std::string>();
                    dp.oad         = p.at("oad").get<std::string>();
                    dp.scale       = JG(p,"scale",  1.0f);
                    dp.offset      = JG(p,"offset", 0.0f);
                    dp.unit        = JG(p,"unit",        std::string(""));
                    dp.description = JG(p,"description", std::string(""));
                    mc.points.push_back(std::move(dp));
                }
            c.meters.push_back(std::move(mc));
        }
    return c;
}

static CanConfig parseCan(const json& m) {
    CanConfig c;
    c.interface        = JG(m,"interface",        c.interface);
    c.fd               = JG(m,"fd",               c.fd);
    c.poll_interval    = JG(m,"poll_interval",    c.poll_interval);
    c.stale_timeout_ms = JG(m,"stale_timeout_ms", c.stale_timeout_ms);
    if (m.contains("signals"))
        for (auto& p : m["signals"]) {
            CanSignal s;
            s.name        = p.at("name").get<std::string>();
            s.can_id      = parseCanId(p.at("can_id"));
            s.extended    = JG(p,"extended",  false);
            s.start_bit   = p.at("start_bit").get<int>();
            s.bit_length  = p.at("bit_length").get<int>();
            s.byte_order  = parseByteOrder(JG(p,"byte_order",std::string("little")));
            s.is_signed   = JG(p,"is_signed", false);
            s.scale       = JG(p,"scale",  1.0);
            s.offset      = JG(p,"offset", 0.0);
            s.unit        = JG(p,"unit",        std::string(""));
            s.description = JG(p,"description", std::string(""));

            // 尽早报错：位域非法在运行时只会静默变成 BAD 点，难排查
            if (s.bit_length < 1 || s.bit_length > 64)
                throw std::runtime_error("CAN 信号 " + s.name + " bit_length 必须在 1..64");
            if (s.start_bit < 0 || s.start_bit > 511)
                throw std::runtime_error("CAN 信号 " + s.name + " start_bit 越界");
            const uint32_t id_max = s.extended ? 0x1FFFFFFFu : 0x7FFu;
            if (s.can_id > id_max)
                throw std::runtime_error("CAN 信号 " + s.name + " can_id 超出"
                    + (s.extended ? "29" : "11") + " 位范围");

            c.signals.push_back(std::move(s));
        }
    return c;
}

// ── appConfigFromJson (内部) ──────────────────────────────────────────────────
static AppConfig appConfigFromJson(const json& j) {
    AppConfig cfg;

    if (j.contains("mqtt")) {
        auto& m = j["mqtt"];
        cfg.mqtt.broker       = JG(m,"broker",       cfg.mqtt.broker);
        cfg.mqtt.port         = JG(m,"port",          cfg.mqtt.port);
        cfg.mqtt.client_id    = JG(m,"client_id",     cfg.mqtt.client_id);
        cfg.mqtt.username     = JG(m,"username",      cfg.mqtt.username);
        cfg.mqtt.password     = JG(m,"password",      cfg.mqtt.password);
        cfg.mqtt.topic_prefix = JG(m,"topic_prefix",  cfg.mqtt.topic_prefix);
        cfg.mqtt.qos          = JG(m,"qos",           cfg.mqtt.qos);
        cfg.mqtt.retain       = JG(m,"retain",        cfg.mqtt.retain);
        cfg.mqtt.keepalive    = JG(m,"keepalive",     cfg.mqtt.keepalive);
    }
    if (j.contains("devices")) {
        for (auto& d : j["devices"]) {
            DeviceEntry entry;
            entry.id      = d.at("id").get<std::string>();
            const std::string protoRaw  = d.at("protocol").get<std::string>();
            const std::string protoName = normProtocol(protoRaw);
            entry.enabled = JG(d,"enabled", true);
            entry.protocol = parseProtocol(protoName, protoRaw, entry.id);
            entry.reconnect_interval_sec = JG(d,"reconnect_interval_sec", 5);
            switch (entry.protocol) {
                case Protocol::MODBUS:
                    if (d.contains("modbus"))   entry.modbus   = parseModbus(d["modbus"]);
                    break;
                case Protocol::IEC104:
                    if (d.contains("iec104"))   entry.iec104   = parseIEC104(d["iec104"]);
                    break;
                case Protocol::IEC61850:
                    if (d.contains("iec61850")) entry.iec61850 = parseIEC61850(d["iec61850"]);
                    break;
                case Protocol::OPCUA:
                    if (d.contains("opcua"))    entry.opcua    = parseOpcUa(d["opcua"]);
                    break;
                case Protocol::DLT645:
                    if (d.contains("dlt645"))   entry.dlt645   = parseDlt645(d["dlt645"]);
                    break;
                case Protocol::DLT698:
                    if (d.contains("dlt698"))   entry.dlt698   = parseDlt698(d["dlt698"]);
                    break;
                case Protocol::CAN:
                    if      (d.contains("can"))   entry.can = parseCan(d["can"]);
                    else if (d.contains("canfd")) entry.can = parseCan(d["canfd"]);
                    if (protoName == "canfd") entry.can.fd = true;   // 简写形式
                    break;
            }
            cfg.devices.push_back(std::move(entry));
        }
    }
    if (j.contains("web")) {
        auto& m = j["web"];
        cfg.web.port       = JG(m,"port",       cfg.web.port);
        cfg.web.bind       = JG(m,"bind",       cfg.web.bind);
        cfg.web.enabled    = JG(m,"enabled",    cfg.web.enabled);
        cfg.web.debug      = JG(m,"debug",      cfg.web.debug);
        cfg.web.debug_root = JG(m,"debug_root", cfg.web.debug_root);
        cfg.web.tls_enabled= JG(m,"tls_enabled",cfg.web.tls_enabled);
        cfg.web.tls_cert   = JG(m,"tls_cert",   cfg.web.tls_cert);
        cfg.web.tls_key    = JG(m,"tls_key",    cfg.web.tls_key);
        cfg.web.auth_enabled  = JG(m,"auth_enabled",  cfg.web.auth_enabled);
        cfg.web.auth_user     = JG(m,"auth_user",     cfg.web.auth_user);
        cfg.web.auth_password = JG(m,"auth_password", cfg.web.auth_password);
    }
    if (j.contains("logging")) {
        auto& m = j["logging"];
        cfg.logging.level        = JG(m,"level",        cfg.logging.level);
        cfg.logging.file         = JG(m,"file",         cfg.logging.file);
        cfg.logging.max_bytes    = JG(m,"max_bytes",    cfg.logging.max_bytes);
        cfg.logging.backup_count = JG(m,"backup_count", cfg.logging.backup_count);
    }
    if (j.contains("cache")) {
        auto& m = j["cache"];
        cfg.cache.enabled         = JG(m,"enabled",         cfg.cache.enabled);
        cfg.cache.db_path         = JG(m,"db_path",         cfg.cache.db_path);
        cfg.cache.retention_hours = JG(m,"retention_hours", cfg.cache.retention_hours);
        cfg.cache.max_rows        = JG(m,"max_rows",        cfg.cache.max_rows);
        cfg.cache.history_topic   = JG(m,"history_topic",   cfg.cache.history_topic);
        cfg.cache.command_topic   = JG(m,"command_topic",   cfg.cache.command_topic);
        cfg.cache.replay_rate_ms  = JG(m,"replay_rate_ms",  cfg.cache.replay_rate_ms);
        cfg.cache.replay_chunk    = JG(m,"replay_chunk",    cfg.cache.replay_chunk);
    }
    if (cfg.cache.history_topic.empty())
        cfg.cache.history_topic = cfg.mqtt.topic_prefix + "/history";
    if (cfg.cache.command_topic.empty())
        cfg.cache.command_topic = cfg.mqtt.topic_prefix + "/cmd/replay";
    return cfg;
}

// ── appConfigToJson (内部) ────────────────────────────────────────────────────
static json appConfigToJson(const AppConfig& cfg) {
    json j;
    j["mqtt"] = {{"broker",cfg.mqtt.broker},{"port",cfg.mqtt.port},
        {"client_id",cfg.mqtt.client_id},{"username",cfg.mqtt.username},
        {"password",cfg.mqtt.password},{"topic_prefix",cfg.mqtt.topic_prefix},
        {"qos",cfg.mqtt.qos},{"retain",cfg.mqtt.retain},
        {"keepalive",cfg.mqtt.keepalive}};

    json devArr = json::array();
    for (auto& e : cfg.devices) {
        json d;
        d["id"]       = e.id;
        d["enabled"]  = e.enabled;
        d["protocol"] = protoStr(e.protocol);
        d["reconnect_interval_sec"] = e.reconnect_interval_sec;
        switch (e.protocol) {
            case Protocol::MODBUS: {
                // 只序列化手配点（from_range=false），展开点由 ranges[] 重建
                json pts = json::array();
                for (auto& p : e.modbus.points)
                    if (!p.from_range)
                        pts.push_back({{"name",p.name},{"address",p.address},
                            {"func_code",p.func_code},{"data_type",dataTypeStr(p.data_type)},
                            {"scale",p.scale},{"offset",p.offset},
                            {"unit",p.unit},{"description",p.description}});
                json rngs = json::array();
                for (auto& r : e.modbus.ranges)
                    rngs.push_back({{"name_prefix",r.name_prefix},{"addr_start",r.addr_start},
                        {"count",r.count},{"func_code",r.func_code},
                        {"data_type",dataTypeStr(r.data_type)},
                        {"scale",r.scale},{"offset",r.offset},
                        {"unit",r.unit},{"description",r.description}});
                d["modbus"] = {{"host",e.modbus.host},{"port",e.modbus.port},
                    {"unit_id",e.modbus.unit_id},{"timeout",e.modbus.timeout_sec},
                    {"poll_interval",e.modbus.poll_interval},
                    {"publish_mode",e.modbus.publish_mode},
                    {"points",pts},{"ranges",rngs}};
                break;
            }
            case Protocol::IEC104: {
                json pts = json::array();
                for (auto& p : e.iec104.points)
                    pts.push_back({{"name",p.name},{"ioa",p.ioa},{"type_id",p.type_id},
                        {"scale",p.scale},{"offset",p.offset},
                        {"unit",p.unit},{"description",p.description}});
                d["iec104"] = {{"host",e.iec104.host},{"port",e.iec104.port},
                    {"common_address",e.iec104.common_address},
                    {"poll_interval",e.iec104.poll_interval},
                    {"timeout",e.iec104.timeout_sec},{"points",pts}};
                break;
            }
            case Protocol::IEC61850: {
                json pts = json::array();
                for (auto& p : e.iec61850.points)
                    pts.push_back({{"name",p.name},{"object_ref",p.object_ref},
                        {"fc",p.fc},{"scale",p.scale},{"offset",p.offset},
                        {"unit",p.unit},{"description",p.description}});
                d["iec61850"] = {{"host",e.iec61850.host},{"port",e.iec61850.port},
                    {"poll_interval",e.iec61850.poll_interval},
                    {"timeout_ms",e.iec61850.timeout_ms},
                    {"auth_password",e.iec61850.auth_password},
                    {"use_reports",e.iec61850.use_reports},{"points",pts}};
                break;
            }
            case Protocol::OPCUA: {
                json pts = json::array();
                for (auto& p : e.opcua.points)
                    pts.push_back({{"name",p.name},{"node_id",p.node_id},
                        {"scale",p.scale},{"offset",p.offset},
                        {"unit",p.unit},{"description",p.description}});
                d["opcua"] = {{"endpoint_url",e.opcua.endpoint_url},
                    {"poll_interval",e.opcua.poll_interval},
                    {"timeout_ms",e.opcua.timeout_ms},
                    {"security_policy",e.opcua.security_policy},
                    {"username",e.opcua.username},{"password",e.opcua.password},
                    {"points",pts}};
                break;
            }
            case Protocol::DLT645: {
                json mts = json::array();
                for (auto& m : e.dlt645.meters) {
                    json pts = json::array();
                    for (auto& p : m.points)
                        pts.push_back({{"name",p.name},{"data_id",p.data_id},
                            {"scale",p.scale},{"offset",p.offset},
                            {"unit",p.unit},{"description",p.description}});
                    mts.push_back({{"id",m.id},{"meter_address",m.meter_address},
                        {"points",pts}});
                }
                d["dlt645"] = {{"connection_type",e.dlt645.connection_type},
                    {"serial_port",e.dlt645.serial_port},{"baud_rate",e.dlt645.baud_rate},
                    {"host",e.dlt645.host},{"tcp_port",e.dlt645.tcp_port},
                    {"poll_interval",e.dlt645.poll_interval},
                    {"timeout_ms",e.dlt645.timeout_ms},{"meters",mts}};
                break;
            }
            case Protocol::DLT698: {
                json mts = json::array();
                for (auto& m : e.dlt698.meters) {
                    json pts = json::array();
                    for (auto& p : m.points)
                        pts.push_back({{"name",p.name},{"oad",p.oad},
                            {"scale",p.scale},{"offset",p.offset},
                            {"unit",p.unit},{"description",p.description}});
                    mts.push_back({{"id",m.id},{"meter_address",m.meter_address},
                        {"points",pts}});
                }
                d["dlt698"] = {{"connection_type",e.dlt698.connection_type},
                    {"serial_port",e.dlt698.serial_port},{"baud_rate",e.dlt698.baud_rate},
                    {"host",e.dlt698.host},{"tcp_port",e.dlt698.tcp_port},
                    {"poll_interval",e.dlt698.poll_interval},
                    {"timeout_ms",e.dlt698.timeout_ms},{"meters",mts}};
                break;
            }
            case Protocol::CAN: {
                json sigs = json::array();
                for (auto& s : e.can.signals)
                    sigs.push_back({{"name",s.name},{"can_id",canIdHex(s.can_id)},
                        {"extended",s.extended},{"start_bit",s.start_bit},
                        {"bit_length",s.bit_length},
                        {"byte_order",byteOrderStr(s.byte_order)},
                        {"is_signed",s.is_signed},
                        {"scale",s.scale},{"offset",s.offset},
                        {"unit",s.unit},{"description",s.description}});
                d["can"] = {{"interface",e.can.interface},{"fd",e.can.fd},
                    {"poll_interval",e.can.poll_interval},
                    {"stale_timeout_ms",e.can.stale_timeout_ms},{"signals",sigs}};
                break;
            }
        }
        devArr.push_back(std::move(d));
    }
    j["devices"] = devArr;
    j["cache"] = {{"enabled",cfg.cache.enabled},{"db_path",cfg.cache.db_path},
        {"retention_hours",cfg.cache.retention_hours},
        {"max_rows",cfg.cache.max_rows},
        {"history_topic",cfg.cache.history_topic},
        {"command_topic",cfg.cache.command_topic},
        {"replay_rate_ms",cfg.cache.replay_rate_ms},
        {"replay_chunk",cfg.cache.replay_chunk}};
    j["web"]     = {{"port",cfg.web.port},{"bind",cfg.web.bind},{"enabled",cfg.web.enabled},
                    {"debug",cfg.web.debug},{"debug_root",cfg.web.debug_root},
                    {"tls_enabled",cfg.web.tls_enabled},{"tls_cert",cfg.web.tls_cert},
                    {"tls_key",cfg.web.tls_key},
                    {"auth_enabled",cfg.web.auth_enabled},{"auth_user",cfg.web.auth_user},
                    {"auth_password",cfg.web.auth_password}};
    j["logging"] = {{"level",cfg.logging.level},{"file",cfg.logging.file},
        {"max_bytes",cfg.logging.max_bytes},
        {"backup_count",cfg.logging.backup_count}};
    return j;
}

// ── loadConfig ────────────────────────────────────────────────────────────────
AppConfig loadConfig(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) throw std::runtime_error("无法打开配置文件: " + path);
    json j;
    try { f >> j; }
    catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("JSON 解析错误: ") + e.what());
    }
    AppConfig cfg = appConfigFromJson(j);
    // 允许 devices[] 为空：--from-db 模式下采集内容全部来自编译快照，配置文件
    // 只提供 web/mqtt/logging（见 examples/ess.config.json）。"空 devices 且没加
    // --from-db"这种真正没意义的组合由 main 判断并明确报错 —— 在这里一刀切地
    // 拒绝，等于逼着那类配置写一个假设备进去。
    if (!j.contains("devices"))
        throw std::runtime_error("配置文件中没有 devices[]，请检查格式");
    return cfg;
}

// ── saveConfig ────────────────────────────────────────────────────────────────
// 原子写：临时文件 → fsync → rename → fsync 目录。
// 直接覆写 path 的话，写到一半掉电会留下截断的 JSON，下次启动 loadConfig 抛异常、
// 程序起不来（无人值守设备等于变砖）。rename 是同一文件系统内的原子替换，
// 读者要么看到旧的完整配置，要么看到新的完整配置。
void saveConfig(const AppConfig& cfg, const std::string& path) {
    const std::string data = appConfigToJson(cfg).dump(2);
    const std::string tmp  = path + ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) throw std::runtime_error("无法写配置: " + tmp);
        f << data;
        f.flush();
        if (!f) { std::remove(tmp.c_str()); throw std::runtime_error("写配置失败: " + tmp); }
    }

    // 先让数据真正落盘，再 rename——否则 rename 落了盘而内容还在页缓存
    int fd = ::open(tmp.c_str(), O_RDONLY);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int e = errno;
        std::remove(tmp.c_str());
        throw std::runtime_error("无法替换配置 " + path + ": " + std::strerror(e));
    }

    // 目录项本身也要落盘，rename 才算持久
    std::string dir = path.substr(0, path.find_last_of('/'));
    if (dir.empty() || dir == path) dir = ".";
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
}

// ── 公共内存序列化/反序列化 ───────────────────────────────────────────────────
std::string appConfigToJsonString(const AppConfig& cfg) {
    return appConfigToJson(cfg).dump(2);
}

AppConfig appConfigFromJsonString(const std::string& s) {
    json j;
    try { j = json::parse(s); }
    catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("JSON 解析错误: ") + e.what());
    }
    if (!j.contains("devices") || j["devices"].empty())
        throw std::runtime_error("devices[] 不能为空");
    return appConfigFromJson(j);
}

} // namespace industrial
