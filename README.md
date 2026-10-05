# Industrial Collector v1.6.0

> 工业数据采集程序：七协议 → MQTT + 内置 Web 管理
> 面向嵌入式 Linux / Ubuntu 18，全静态链接，**零运行时依赖**

---

## 支持协议

| 协议 | 标准 | 典型场景 | 默认端口 |
|------|------|---------|---------|
| **Modbus TCP** | IEC 60870 | PLC、变频器、仪表 | 502 |
| **IEC 60870-5-104** | IEC 60870 | 电力 RTU、变电站 | 2404 |
| **IEC 61850 MMS** | IEC 61850 | 智能变电站、保护装置 | 102 |
| **OPC UA** | IEC 62541 | 现代 PLC、SCADA 服务器 | 4840 |
| **DL/T 645-2007** | DL/T 645 | 单相/三相电表（RS485 总线 / TCP 网关） | — |
| **DL/T 698.45** | DL/T 698 | 智能电表（RS485 总线 / TCP 网关） | — |
| **CAN / CAN FD** | ISO 11898 | BMS、PCS、变流器（SocketCAN 被动监听） | — |

**混合协议**：单进程可同时采集多种协议；DLT645/DLT698 采用"总线模型"
（一条总线一个线程，挂载多块表），支持串口直连与 TCP 网关两种接线方式。

---

## 主要特性

| 功能 | 说明 |
|------|------|
| **零依赖部署** | 全静态链接（libmodbus / paho / libiec61850 / open62541 / sqlite3 / OpenSSL 均以 `.a` 编入） |
| **自动重连** | 所有协议按 `reconnect_interval_sec` 无限重连，断线不中断其他设备 |
| **Web 管理** | AdminLTE 风格三页 SPA：实时监控 / 配置管理 / 系统日志（内容区浅色主题） |
| **SSE 实时推送** | 监控页通过 `GET /api/stream` SSE 长连接接收推送，设备数据更新后即时到达，无轮询开销 |
| **配置 CRUD** | Web UI 完整增删改设备 & 采集点，`POST /api/config/full` 保存到 JSON |
| **HTTPS** | 静态 OpenSSL 1.1.1w，`bash scripts/gen_cert.sh` 生成自签证书，证书无效自动回退 HTTP |
| **Web 鉴权** | HTTP Basic，`web.auth_enabled` 开启后静态页与全部 `/api/*` 均需认证；未开鉴权且非本地监听时启动告警 |
| **Debug 前端** | `--debug` 或 `web.debug:true`，从磁盘 `web/` 实时伺服，改前端无需重编 |
| **本地缓存** | SQLite 批量写入（16 条/次 或 2s 定时提交，I/O 减少 10x），MQTT 离线期间持续写入 |
| **断点续传** | 收到指令后按游标/时间段回放缓存，支持掉线保存断点 |
| **glibc 兼容** | `src/compat.c` 提供 `strlcpy` / `__isoc23_strtol` 等符号，可在 Ubuntu 20（glibc 2.31）直接编译 |

---

## 快速开始（Ubuntu 18.04）

```bash
# 1. 解压
tar xzf industrial_collector-v1.5.0.tgz && cd industrial_collector

# 2. 一键编译（首次约 20~25 分钟，下载并静态编译所有依赖）
bash scripts/bootstrap.sh

# 3. 配置（修改设备 IP、选择协议）
nano config/config.json

# 4. 运行
./build/industrial_collector config/config.json

# 5. 浏览器打开 Web 管理界面
#    http://<设备IP>:15688      （端口来自 config.json 的 web.port）
```

---

## 启动与停止

三种启动场景，按需选。

### 场景一：单实例，最普通

配置文件里 `devices[]` 直接写死，跟 v1.x 完全一样：

```bash
./build/industrial_collector config/config.json
# 打开 http://127.0.0.1:15688 （config.json 里 web.port 决定）
```

加两个开发期常用的开关：

```bash
./build/industrial_collector config/config.json --debug --web-port 15688
#                                                ^^^^^^^  ^^^^^^^^^^^^^^^^
#                                                web/ 磁盘  覆盖配置里的端口
#                                                实时伺服，
#                                                改前端不用重编
```

### 场景二：`--from-db`，走"设备规格 + 设备树"

设备表从 `config.db` 的**编译快照**读，`devices[]` 可以是空的。适用于储能站这类
测点是万级、必须走"设备规格实例化 + 「生效」编译"的场景：

```bash
# 头一次：把 examples 里的储能站示例导入库
bash examples/ess_models.sh                 # 建 6 个设备规格 + 4×RACK001 + 4×RACK002
curl -s -X POST http://127.0.0.1:15688/api/compile -d '{}'   # 编出 v1 快照

# 然后正式跑：
./build/industrial_collector examples/ess.config.json --from-db --debug
```

`ess.config.json` 里 `devices[]` 故意留空—— main 会明确报错并提示要加
`--from-db`，避免"忘记加参数结果谁都不采"这种静默失败。

### 场景三：六协议测试环境（Docker 模拟器 + 本机采集器）

一键起完整测试环境（MQTT broker、六协议模拟器、IEC61850 服务端、DLT645 PTY 桥）：

```bash
bash test/run_test.sh up      # 起模拟器 + 桥
bash test/run_test.sh run     # 前台跑采集器，Ctrl-C 停
bash test/run_test.sh sub     # 另开一个窗口订阅 MQTT 看数据
bash test/run_test.sh down    # 停全部（模拟器 + 桥；采集器 Ctrl-C 自己停）
```

或者手工同时跑两个实例，一个跑六协议、一个跑储能站：

```bash
bash test/run_test.sh up
./build/industrial_collector test/docker/config.docker.json --debug &   # 15647
./build/industrial_collector examples/ess.config.json --from-db --debug &  # 15688
```

### 停止 ⚠️

**别用 `pkill -f industrial_collector`**——这个模式会匹到发起命令的 shell 自身
的命令行，把 shell 也一起打死（本项目多次踩过这个坑）。用**监听端口反查 PID**，
再核对 `/proc/PID/exe` 确认是本二进制，然后 `kill -TERM`：

```bash
# 停单个端口的实例
port=15688
pid=$(ss -lntpH "sport = :$port" | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2)
[ "$(readlink /proc/$pid/exe | sed 's/ (deleted)//')" \
   = "$(realpath ./build/industrial_collector)" ] \
  && kill -TERM $pid \
  || echo "  ⚠ pid $pid 不是本采集器，跳过"
```

或者用 `test/run_test.sh` 的封装：

```bash
bash test/run_test.sh down
```

**关于 SIGTERM 的响应**：主循环里的三种采集器（Modbus/IEC104/…、DLT 总线、CAN
被动监听）都用 200ms 分片的可中断 sleep，SIGTERM 到达后**亚秒级**退出（实测
最长 1.4 秒，一次连接尝试的超时）。若 60 秒仍未退出再 `kill -KILL`——那多半
是链路层卡在 recv，属需查的问题。

### 常用启动参数

| 参数 | 作用 |
|---|---|
| `--web-port PORT` | 覆盖 `config.web.port`，其他一律走文件 |
| `--debug` | 前端从磁盘 `web/` 实时伺服，改完刷新即可，无需重编（生产别开） |
| `--from-db` | 采集源改用 `config.db` 的编译快照；未加时仍按 `config.json.devices[]` 采集 |

以下不带任何参数，默认读 `config/config.json`：

```bash
./build/industrial_collector             # ← 等价于 ./build/industrial_collector config/config.json
```

---

## 项目结构

```
industrial_collector/
├── CMakeLists.txt
├── TODO.md                          功能清单（全部已交付）
├── config/
│   └── config.json                  完整配置示例（六协议混合）
├── include/
│   ├── types.h                      公共数据结构（六协议点定义）
│   ├── config.h                     配置结构体 + appConfigTo/FromJsonString
│   ├── data_cache.h                 SQLite 缓存接口
│   ├── mqtt_publisher.h             MQTT 发布 + 缓存 + 断点续传
│   ├── web_server.h                 SharedState + Web 服务
│   ├── io_transport.h               ITransport（串口 / TCP）
│   ├── modbus_collector.h
│   ├── iec104_collector.h
│   ├── iec61850_collector.h
│   ├── opcua_collector.h
│   ├── dlt645_collector.h
│   └── dlt698_collector.h
├── src/
│   ├── main.cpp                     多设备线程调度、信号处理
│   ├── config.cpp                   加载/保存/内存序列化配置
│   ├── data_cache.cpp               SQLite WAL 缓存（批量写入 / prune）
│   ├── mqtt_publisher.cpp           连接 + 缓存写入 + 回放线程
│   ├── web_server.cpp               HTTP/HTTPS 路由（/api/all + /api/stream SSE 等）
│   ├── compat.c                     glibc 2.31 兼容符号（strlcpy / __isoc23_strtol）
│   ├── modbus_collector.cpp
│   ├── iec104_collector.cpp
│   ├── iec61850_collector.cpp
│   ├── opcua_collector.cpp
│   ├── dlt645_collector.cpp
│   ├── dlt698_collector.cpp
│   └── io_transport.cpp
├── web/                             前端源码（--debug 或开发时使用）
│   ├── index.html                   AdminLTE 风格三页 SPA
│   └── assets/
│       ├── style.css                深色 sidebar/header + 浅色内容区混合主题
│       ├── app.js                   监控页逻辑 + SSE + 页面路由
│       └── config.js                配置 CRUD（设备增删改 + 点表）
├── scripts/
│   ├── bootstrap.sh                 一键编译脚本（12 步）
│   ├── gen_cert.sh                  生成 HTTPS 自签证书
│   ├── gen_web.py                   编译时打包 web/ 为 C++ 字节数组
│   └── install_service.sh           systemd 服务安装
├── test/
│   ├── TESTING.md                   测试说明（端口配置 / 各功能验证）
│   ├── run_test.sh                  一键测试脚本
│   ├── dlt_frame_test.cpp           DLT645/698 字节级单元测试
│   ├── docker/                      MQTT broker + 五协议 Python 模拟器
│   └── iec61850_server/             libiec61850 server_example（宿主机跑）
└── third_party/                     由 bootstrap.sh 自动填充
    ├── nlohmann/json.hpp            v3.11.3  header-only
    ├── httplib.h                    v0.12.6  header-only（版本锁定）
    ├── spdlog/                      v1.13.0  header-only
    ├── install/
    │   ├── libmodbus/               v3.1.10  静态库
    │   ├── paho-c/                  v1.3.13  静态库
    │   ├── paho-cpp/                v1.3.2   静态库
    │   ├── libiec61850/             v1.5.3   静态库
    │   ├── open62541/               v1.3.9   静态库（amalgamation）
    │   ├── sqlite3/                 3.45.x   静态库（amalgamation）
    │   └── openssl/                 1.1.1w   静态库（SSL + Crypto）
    └── …-src/                       各库源码（bootstrap.sh 下载）
```

---

## 配置文件详解

配置文件为 JSON，程序运行期间也可通过 **Web 配置页** 进行可视化编辑。

### 顶层结构

```json
{
  "mqtt":    { ... },
  "web":     { ... },
  "cache":   { ... },
  "logging": { ... },
  "devices": [ ... ]
}
```

### MQTT

```jsonc
"mqtt": {
  "broker":       "192.168.1.10",
  "port":         1883,
  "client_id":    "industrial_collector",
  "username":     "",
  "password":     "",
  "topic_prefix": "factory/line1",
  "qos":          1,
  "retain":       false,
  "keepalive":    60
}
```

### Web 服务

```jsonc
"web": {
  "port":        8080,
  "bind":        "0.0.0.0",
  "enabled":     true,
  "debug":       false,         // true = 从磁盘 web/ 实时伺服（开发用）
  "debug_root":  "",            // 空 = 使用编译期注入的 web/ 路径
  "tls_enabled": false,         // 是否启用 HTTPS
  "tls_cert":    "cert.pem",
  "tls_key":     "key.pem",
  "auth_enabled":  false,       // true = 全站 HTTP Basic 鉴权
  "auth_user":     "admin",
  "auth_password": ""           // 开启鉴权时不得为空，否则 Web 拒绝启动
}
```

**启用 HTTPS**：

```bash
bash scripts/gen_cert.sh   # 在当前目录生成 cert.pem / key.pem（有效期 10 年）
# 然后在 config.json 的 web 段设置 "tls_enabled": true
```

**启用鉴权**（强烈建议，只要不是仅监听 127.0.0.1）：

```jsonc
"web": { "auth_enabled": true, "auth_user": "admin", "auth_password": "改成强密码" }
```

```bash
curl -u admin:改成强密码 http://<设备IP>:8080/api/status
```

- 开启后**所有**路由都要认证 —— 静态页和全部 `/api/*`。只保护写接口是不够的：
  `/api/logs`、`/api/points` 同样会泄漏现场数据。
- `POST /api/control {"action":"stop"}` 能停掉采集程序、`POST /api/config/full` 能改写整个配置。
  **不开鉴权 = 任何能访问该端口的人都能停你的产线。** 未开鉴权且 `bind` 非回环时，启动会打印安全警告。
- `auth_enabled: true` 而 `auth_password` 为空时，Web 服务**拒绝启动**（失败关闭），采集不受影响。
- Basic 凭据仅做 base64 编码，不是加密。公网暴露请**同时**开启 `tls_enabled`；未开 TLS 时启动会告警。
- 密码明文存于 `config.json`（与 `mqtt.password` 一致）→ 建议 `chmod 600 config/config.json`。

**配置页里的密码显示为 `********`**：`GET /api/config/full` 不回显任何明文密码
（`mqtt.password` / `web.auth_password` / `opcua.password` / `iec61850.auth_password`）。
保存时若字段仍是 `********`，后端会换回原值；想改密码就直接覆盖它，想清空就留空。

### 本地缓存 / 断点续传

```jsonc
"cache": {
  "enabled":        true,
  "db_path":        "cache.db",
  "retention_hours": 24,
  "max_rows":       1000000,
  "history_topic":  "",          // 空 = {prefix}/history
  "command_topic":  "",          // 空 = {prefix}/cmd/replay
  "replay_rate_ms": 50,          // 每条回放间隔（限速）
  "replay_chunk":   200          // 每次查询块大小
}
```

**续传指令（发到 command_topic）**：

```jsonc
{}                                 // 普通续传：从断点继续，发完推进断点
{"since": 1234}                    // 从指定 rowid 续传（不动断点）
{"start":"2024-01-01T00:00:00Z",   // 按时间段回放（不动断点）
 "end":  "2024-01-01T06:00:00Z"}
```

### 设备条目

```json
{
  "id":                    "plc_1",
  "enabled":               true,
  "protocol":              "modbus",
  "reconnect_interval_sec": 5,
  "modbus": { ... }
}
```

支持的 `protocol` 值：`modbus` / `iec104` / `iec61850` / `opcua` / `dlt645` / `dlt698` / `can`（`canfd` 为 `can` + `fd:true` 的简写）

大小写与首尾空白会被忽略（`"CAN"`、`" canfd "` 均可）。**未知的 protocol 值会导致启动失败**并列出可选值——
不会静默回退成 Modbus。

### Modbus TCP 点配置

```json
{
  "name": "temperature", "address": 0, "func_code": 3,
  "data_type": "float",  "scale": 0.1, "offset": 0.0,
  "unit": "℃",           "description": "炉温"
}
```

`data_type`：`bool` / `int16` / `uint16` / `int32` / `uint32` / `float`

### IEC 61850 点配置

```json
{
  "name": "phase_voltage_a",
  "object_ref": "MEAS/MMXU1.PhV.phsA.cVal.mag.f",
  "fc": "MX", "scale": 1.0, "unit": "V"
}
```

常用功能约束（FC）：`MX`（测量）/ `ST`（状态）/ `CF`（配置）/ `CO`（控制）

### OPC UA 点配置

```json
{ "name": "motor_speed", "node_id": "ns=2;s=MotorSpeed", "unit": "rpm" }
```

NodeId 格式：`ns=2;i=1001`（数字型）/ `ns=2;s=Temperature`（字符串型）

### DL/T 645 总线配置

```json
{
  "id": "rs485_bus_1", "protocol": "dlt645",
  "dlt645": {
    "connection_type": "serial",
    "serial_port": "/dev/ttyUSB0", "baud_rate": 9600,
    "poll_interval": 5.0, "timeout_ms": 3000,
    "meters": [
      {
        "id": "meter_A", "meter_address": "000000000001",
        "points": [
          { "name": "total_energy", "data_id": "00010000", "scale": 0.01, "unit": "kWh" },
          { "name": "voltage_a",    "data_id": "02010100", "scale": 0.1,  "unit": "V"   }
        ]
      }
    ]
  }
}
```

TCP 网关用 `"connection_type": "tcp"` 并填 `host` / `tcp_port`（字段名 **`tcp_port`**，不是 `port`）。

### DL/T 698.45 总线配置

结构与 DLT645 相同，点配置用 `oad`（8位十六进制 OAD）替代 `data_id`：

```json
{ "name": "voltage_a", "oad": "02010100", "scale": 1.0, "unit": "V" }
```

### CAN / CAN FD 配置

CAN 是广播总线，没有请求/应答。采集器**被动监听**：后台线程持续收帧，按 CAN ID 只保留
最新一帧；每个 `poll_interval` 从缓存解码全部信号并上报。

```json
{
  "id": "bms_can",
  "protocol": "can",
  "can": {
    "interface": "can0",
    "fd": false,
    "poll_interval": 1.0,
    "stale_timeout_ms": 5000,
    "signals": [
      { "name": "pack_voltage", "can_id": "0x180", "start_bit": 0,  "bit_length": 16,
        "byte_order": "little", "is_signed": false,
        "scale": 0.1, "offset": 0.0, "unit": "V", "description": "电池组总电压" },
      { "name": "pack_current", "can_id": "0x180", "start_bit": 16, "bit_length": 16,
        "byte_order": "little", "is_signed": true,
        "scale": 0.1, "offset": 0.0, "unit": "A", "description": "电流（放电为负）" }
    ]
  }
}
```

| 字段 | 说明 |
|------|------|
| `interface` | SocketCAN 网络接口名，如 `can0` / `vcan0` |
| `fd` | `true` = CAN FD（负载最长 64 字节）。`protocol:"canfd"` 等价于 `fd:true` |
| `stale_timeout_ms` | 帧有效期：距上次收到超过该值的信号标记为 `BAD`。`0` = 永不过期 |
| `can_id` | 数字或十六进制字符串（`"0x18FF50E5"`） |
| `extended` | `true` = 29 位扩展帧。标准帧 `0x180` 与扩展帧 `0x180` 视为不同帧 |
| `start_bit` / `bit_length` | DBC 位编号：位号 = 字节序号×8 + 字节内位序号（0 = 该字节 LSB）。`bit_length` 取 1..64 |
| `byte_order` | `little`（Intel，`start_bit` 是信号最低位）或 `big`（Motorola，`start_bit` 是最高位） |
| `is_signed` | 按二进制补码解释并符号扩展 |

上报值 = 原始值 × `scale` + `offset`（`raw_value` 字段保留未缩放的原始整数）。

**⚠ 接口比特率必须先由系统拉起** —— RAW socket 无法设置比特率，因此配置里没有
`bitrate` 项（写了也不会生效）：

```bash
# 经典 CAN
sudo ip link set can0 up type can bitrate 500000
# CAN FD（仲裁段 500k / 数据段 2M）
sudo ip link set can0 up type can bitrate 500000 dbitrate 2000000 fd on
# 本地测试用虚拟接口
sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0
```

**关于"掉线"判定**：CAN 与轮询型协议不同 —— **只有 socket 报错才重连**，总线静默不算故障。

轮询协议连续 3 轮全 BAD 说明对端不可达，重连有意义；但被动监听没有请求可发，全 BAD 只说明
此刻没有目标帧在广播，重开 socket 变不出数据，反而会清空帧缓存并留下重连间隔的盲窗。
因此慢速广播的设备**不需要**为了避嫌重连而调大 `poll_interval`，只需把 `stale_timeout_ms`
设为大于最慢帧周期，信号就不会被误判为 BAD。

真正的链路故障仍能捕获：接口被 `ip link set can0 down` 或删除时，`poll()` 置 `POLLERR`、
`read()` 返回 `ENETDOWN` / `ENODEV`，接收线程退出并置位故障标志，下一个采集周期即触发重连
（比原先的 3 轮判定更快）。接口恢复后按 `reconnect_interval_sec` 自动重连成功。

字节序验证：`cd build && make can_signal_test && ./can_signal_test`（手工推导的 DBC 位序期望值，无需真实 CAN 接口）。

---

## MQTT 消息格式

### 单点：`{prefix}/{device_id}/{point_name}`

```json
{
  "name": "temperature", "value": 25.3, "quality": "GOOD",
  "timestamp": "2024-01-15T08:30:00.123Z", "unit": "℃"
}
```

### 批量：`{prefix}/{device_id}/batch`

```json
{
  "timestamp": "...",
  "points": {
    "temperature": {"value": 25.3, "quality": "GOOD", "unit": "℃"},
    "pressure":    {"value": 1.23, "quality": "GOOD", "unit": "MPa"}
  }
}
```

### 历史回放：`{prefix}/history`（缓存续传时）

```json
{"cursor": 1024, "device": "modbus_1", "ts": "...", "topic": "...", "data": {...}}
```

---

## Web 管理界面

浏览器访问 `http://<设备IP>:<web.port>`，AdminLTE 风格暗色主题。

### 实时监控页

- 信息盒子：成功采集 / 错误次数 / MQTT 发布 / 设备状态
- 数据点网格：质量颜色条（绿=GOOD / 红=BAD / 黄=UNCERTAIN）
- 趋势图：60 点滚动历史，下拉切换采集点
- 侧边栏：设备统计 + 采集控制（启动/暂停/停止）

### 配置管理页

- **基础设置**：MQTT / Web 服务 / 缓存 / 日志 四选项卡，保存后立即生效
- **设备管理**：增删改设备 + 采集点（DLT645/698 电表用折叠式两级结构）；
  设备连接参数修改后重启生效

### 系统日志页

- 最近 500 条日志，支持自动滚动

### REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET  | `/api/stream`        | **SSE 长连接**：设备数据更新时即推 `{status,points,logs}`（监控页主数据源） |
| GET  | `/api/all[?n=N]`     | 一次返回 status + points + 最近 N 条日志（单请求，适合脚本查询） |
| GET  | `/api/status`        | 运行状态 + 各设备统计 |
| GET  | `/api/points[?id=X]` | 所有点（或指定设备）最新值 |
| GET  | `/api/logs[?n=N]`    | 最近 N 条日志（默认100，最多500） |
| GET  | `/api/config`        | 当前配置摘要 |
| PUT  | `/api/config`        | 热更新：MQTT topic/QoS、设备轮询周期 |
| GET  | `/api/config/full`   | 完整 AppConfig JSON |
| POST | `/api/config/full`   | 保存完整配置（MQTT/缓存立即生效，设备参数重启后生效） |
| POST | `/api/control`       | `{"action":"pause"}` / `"resume"` / `"stop"` |

---

## 命令行选项

```
./industrial_collector [config.json] [--web-port PORT] [--debug] [--help]

  config.json     配置文件路径（默认 config/config.json）
  --web-port PORT 覆盖配置文件中的 web.port
  --debug         前端从磁盘 web/ 实时伺服（改前端后刷新即可，无需重编）
```

---

## 依赖版本锁定说明

| 库 | 版本 | 锁定原因 |
|----|------|---------|
| cpp-httplib | **v0.12.6** | v0.13+ 依赖 OpenSSL 3，Ubuntu 18 仅 1.1.1 |
| OpenSSL | **1.1.1w** | Ubuntu 18 兼容，静态编入；paho SHA1 符号冲突用 objcopy 重命名 |
| paho.mqtt.c/cpp | v1.3.x | 静态编译；SHA1 符号已用 objcopy 重命名（paho_SHA1_*） |
| open62541 | v1.3.9 | ENCRYPTION=OFF，amalgamation 单文件编译为 .a |
| libiec61850 | v1.5.3 | MBEDTLS=OFF；安装后删除 MSVC shim stdbool.h（否则 sizeof(bool) 断言失败） |
| pymodbus | 3.6.9 | 测试用；3.x 移除了同步 API，3.13 又加了不兼容变更 |
| glibc | ≥ 2.31 | Ubuntu 20 编译时需 `src/compat.c`（提供 `strlcpy`/`__isoc23_strtol`，三方库若在 glibc 2.38+ 下编译会引用这些符号） |

---

## systemd 服务

```bash
bash scripts/install_service.sh

systemctl status industrial-collector
journalctl -fu industrial-collector
```
