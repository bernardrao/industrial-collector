# 测试指南

六协议端到端测试环境（v1.5.0）：**采集器跑宿主机**，MQTT broker 与 Modbus / IEC104 / OPC-UA / DLT645 / DLT698
模拟器跑 **Docker**，IEC61850 测试服务端用 libiec61850 自带 example 在**宿主机本地**起。

> 设计目的：模拟器隔离在容器内，不污染主环境；采集器用已编译的二进制连接发布端口。

---

## 快速开始

```bash
# 一键：编译 + 起模拟器 + 跑采集器
bash test/run_test.sh all

# 或分步：
bash test/run_test.sh build    # 编译采集器 + IEC61850 服务端
bash test/run_test.sh up       # 起 docker 模拟器 + IEC61850 服务端
bash test/run_test.sh run      # 前台跑采集器（Ctrl-C 停）
bash test/run_test.sh sub      # 另开终端，订阅 MQTT 看实时数据
bash test/run_test.sh down     # 收尾，停全部
```

**前置条件**：已运行过 `scripts/bootstrap.sh`，安装了 `docker`，有 `gcc-9` / `g++-9`。

浏览器打开 **http://127.0.0.1:15647** 看 Web 管理界面（6 个设备实时数据）。

> **端口被占时怎么办**：本机若已有别的服务占了 15647 / 15646（`ss -ltnp | grep 156`
> 可查），采集器的 Web 端口用 `--web-port <端口>` 覆盖即可，无需改配置文件；
> IEC61850 服务端的端口直接作为第一个参数传（见下面的实测记录）。

---

## 实测启动记录（2026-08-18，可照抄）

一次完整的手工启动，六个设备全部 `运行中`：

```bash
# 1. 起模拟器容器（MQTT broker + 五个协议模拟器）
cd test/docker && docker compose up -d --build && cd ../..

# 2. 起 IEC61850 服务端（端口作为第一个参数；102 需 root，故用高位端口）
./test/iec61850_server/iec61850_server 15646 &

# 3. 跑采集器（端口全在配置里，无需额外参数）
./build/industrial_collector test/docker/config.docker.json
```

验证：

```bash
curl -s http://127.0.0.1:15647/api/status | python3 -m json.tool | grep -E 'status|good_polls'
curl -s http://127.0.0.1:15647/api/points | python3 -m json.tool | head -20
```

预期六个设备全部 `运行中`，`/api/points` 有 6 个键：

```
dlt645_gw/meter_01   dlt698_gw/smeter_01   iec104_sim
iec61850_srv         modbus_sim            opcua_sim
```

### 容器命名：为什么不叫 ic_mqtt

本机上已有一个名为 **`ic_mqtt`** 的容器，属于**另一个应用**（数据卷
`center_sink_mqtt_data`），只是碰巧复用了本项目构建的 `docker_mqtt` 镜像。
测试栈的 broker 因此改名 **`ic_test_mqtt`** 并独占 15640 端口，与之彻底分开 ——
早先版本共用 15631，等于把测试数据发进了别人的 broker。

若 `docker compose up` 仍报 `container name is already in use`，说明有同名残留：

```bash
docker rm -f ic_test_mqtt ic_sims     # 只删测试栈自己的容器，勿动 ic_mqtt
```

### 停止时不要用 `pkill -f industrial_collector` ⚠️

`pkill -f` 按**整条命令行**匹配，而执行该命令的 shell 自己的命令行里也含这个字符串，
于是把自己一起杀掉——现象是命令莫名中断、后台刚起的进程跟着没了。按可执行文件
真实路径匹配才安全：

```bash
for p in $(ls /proc/[0-9]*/exe 2>/dev/null | cut -d/ -f3); do
  case "$(readlink /proc/$p/exe 2>/dev/null)" in
    */industrial_collector) kill -9 "$p";;
  esac
done
```

同理 `setsid cmd &` 之后 `$!` 拿到的**不是**采集器的 PID（setsid 会二次 fork），
要从监听端口反查：

```bash
ss -ltnp | grep ':15647 ' | grep -oE 'pid=[0-9]+' | cut -d= -f2
```

---

## 手动命令（脚本背后做的事）

```bash
# 1. 编译采集器
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc-9 -DCMAKE_CXX_COMPILER=g++-9 \
    -DTHIRD_PARTY_DIR=../third_party
make -j$(nproc)
cd ..

# 2. 编译 IEC61850 测试服务端
bash test/iec61850_server/build.sh

# 3. 起 MQTT broker + 协议模拟器（Docker）
cd test/docker && docker compose up -d --build   # 老版本用 docker-compose
cd ../..

# 4. 起 IEC61850 服务端（非特权端口 15646；102 需 root）
./test/iec61850_server/iec61850_server 15646 &

# 5. 跑采集器
./build/industrial_collector test/docker/config.docker.json

# 6. 另开终端验证 MQTT
docker exec ic_test_mqtt mosquitto_sub -t 'test/docker/#' -v

# 7. 收尾
cd test/docker && docker compose down
pkill -f "iec61850_server 15646"
```

---

## 端口配置 —— 在哪里改 ⚠️

端口分散在 **3~4 个文件**，改端口必须同步，否则采集器连不上（典型报错 `Connection refused`）。

### 当前端口分配（宿主机 127.0.0.1）

| 服务 | 宿主端口 | 容器内端口 | 用途 |
|------|---------|-----------|------|
| MQTT broker | 15640 | 1883 | 采集器发布 |
| Modbus | 15641 | 1502 | |
| IEC104 | 15642 | 2404 | |
| OPC UA | 15643 | 15643 | 内外同端口（见下注） |
| DLT645 网关 | 15644 | 20108 | |
| DLT698 网关 | 15645 | 20109 | |
| IEC61850 服务端 | 15646 | —（宿主直跑） | 端口作为第一个参数传，可改 |
| Web 管理界面 | 15647 | —（采集器内置） | 浏览器访问，可用 `--web-port` 覆盖 |

> **为何收编到 1564x**：早先端口散在 15631-15637 与 15680 两段，其中 15631 与
> 另一应用的 `ic_mqtt` 容器冲突（详见上文"容器命名"）、15680 被本机
> `edge_web_server` 占用。2026-08-18 整体搬到 **15640-15647** 连续区间：一眼可辨
> 归属，防火墙也能整段放行。
>
> 仍可临时覆盖：Web 用 `--web-port <端口>`；IEC61850 服务端的端口是它的第一个
> 命令行参数，但 `config.docker.json` 的 `iec61850.port` 要跟着改 —— 采集器是照
> 配置去连的，只改服务端启动参数会连不上。

### 要改端口时，改这些位置

**1. `test/docker/docker-compose.yml`** —— 宿主↔容器端口映射
```yaml
ports:
  - "127.0.0.1:15640:1883"     # 宿主端口:容器端口，改冒号左边
  - "127.0.0.1:15641:1502"
  ...
```

**2. `test/docker/config.docker.json`** —— 采集器连接的目标（必须 = compose 宿主端口）
```jsonc
"mqtt":     { "port": 15640 }
"modbus":   { "port": 15641 }            // host 均为 127.0.0.1
"iec104":   { "port": 15642 }
"opcua":    { "endpoint_url": "opc.tcp://127.0.0.1:15643/..." }
"iec61850": { "port": 15646 }
"dlt645":   { "tcp_port": 15644 }        // ★ 易漏！不是 port 而是 tcp_port
"dlt698":   { "tcp_port": 15645 }        // ★ 易漏！
"web":      { "port": 15647 }
```
> 踩坑提醒：DLT 的字段名是 **`tcp_port`** 不是 `port`，remap 端口时最易漏改。

**3. `test/docker/simulators/opcua_sim.py`** —— OPC UA 特殊处理
```python
server.set_endpoint("opc.tcp://0.0.0.0:15643/freeopcua/server/")
```
OPC UA 的 `GetEndpoints` 会把"宣告端口"返回给客户端用于重连，所以**容器内外端口必须一致**
（compose 里映射成 `15643:15643`）。改 OPC UA 端口要同时改 `opcua_sim.py`、
`docker-compose.yml`、`config.docker.json` 三处。

**4. IEC61850 服务端端口** —— 启动参数 + config
```bash
./test/iec61850_server/iec61850_server <端口>   # 脚本中默认 15646
```
对应 `config.docker.json` 的 `iec61850.port` 及 `run_test.sh` 顶部的 `IED_PORT`。

**5. Web 端口** —— 仅在 `config.docker.json` 的 `web.port`（采集器内置，无需 docker 映射）。

---

## 各功能验证

### 1. 六协议实时采集

采集器运行后，观察 Web 监控页数据点颜色（绿=GOOD）或 MQTT 订阅输出。

| 设备 | 点 | 预期值 |
|------|-----|--------|
| modbus_sim | temperature / pressure / pump_speed | 25.5℃ / 1.23MPa / 1450rpm |
| iec104_sim | grid_voltage / grid_current | ~220V / ~15A |
| iec61850_srv | analog_in_1 / analog_in_2 | 正弦波 [-1, 1] |
| opcua_sim | temperature / motor_speed | ~25℃ / 1450rpm |
| dlt645_gw/meter_01 | total_energy / voltage_a | 123.45kWh / 220.1V |
| dlt698_gw/smeter_01 | voltage_a / active_power | 220.1V / 1.2345kW |

### 2. DLT 字节级单元测试

```bash
cd build && make dlt_frame_test && ./dlt_frame_test
```
期望帧按 DL/T 645/698 规约**手工推导**，CRC-16/ARC 用标准向量 `"123456789"→0xBB3D` 锚定。
模拟器与采集器同源（仅自洽），规约符合性由此单元测试独立保证。

### 3. 自动重连

```bash
# 采集器运行中，停掉模拟器容器
docker stop ic_sims
# 日志出现 "[xxx] N 秒后重连…"
# 再启动
docker start ic_sims
# 日志恢复 "[xxx] 采集循环启动"，MQTT 数据恢复 GOOD
```

### 4. Web 配置 CRUD

1. 浏览器打开 `http://127.0.0.1:15647`
2. 侧边栏点「配置管理」
3. 修改 MQTT > Topic 前缀，点「保存基础设置」→ 弹出"配置已保存；MQTT/缓存参数立即生效"
4. 点「添加设备」，选协议 Modbus，填 ID / 主机 / 端口，添加采集点，保存
5. 设备列表出现新行；查看 `config.docker.json` 已更新

也可用 curl 验证：
```bash
# 获取完整配置
curl -s http://127.0.0.1:15647/api/config/full | python3 -m json.tool | head -30

# 保存修改（批量替换 topic_prefix 示例）
curl -s http://127.0.0.1:15647/api/config/full > /tmp/cfg.json
# 编辑 /tmp/cfg.json ...
curl -s -X POST http://127.0.0.1:15647/api/config/full \
     -H 'Content-Type: application/json' -d @/tmp/cfg.json
```

### 5. HTTPS

```bash
# 生成自签证书（使用 third_party 静态 openssl，无需系统 openssl）
bash scripts/gen_cert.sh

# 在 config.docker.json（或 config.json）web 段加：
#   "tls_enabled": true,
#   "tls_cert": "cert.pem",
#   "tls_key": "key.pem"

# 启动采集器，观察日志：
# [INFO] Web HTTPS 已启用 (cert=cert.pem, key=key.pem)
# [INFO] Web 管理界面: https://0.0.0.0:15647

# 验证
curl -k https://127.0.0.1:15647/api/status   # -k 跳过自签证书校验
# 应返回 JSON 状态

# 浏览器访问 https://127.0.0.1:15647（浏览器会提示证书不受信任，继续即可）
```

**证书无效时的行为**（自动回退）：

```bash
# 故意填错路径
# "tls_cert": "notfound.pem"
# 日志：[ERROR] HTTPS 证书/私钥无效 ... 回退明文 HTTP
# curl http://127.0.0.1:15647/api/status  → 仍正常返回
```

### 6. 本地缓存 + 断点续传

```bash
# 确保 config.docker.json 中 cache.enabled = true
# 采集器运行后查看 SQLite 缓存
sqlite3 cache.db "SELECT count(*), min(ts), max(ts) FROM cache;"

# 模拟掉线：停 MQTT broker
docker stop ic_test_mqtt
# 此时采集器日志出现重连，但 cache 继续写入
sqlite3 cache.db "SELECT count(*) FROM cache;"   # 行数持续增长

# 恢复 broker
docker start ic_test_mqtt
# 采集器自动重连，重订阅 command_topic

# 触发普通续传（从断点继续）
docker exec ic_test_mqtt mosquitto_pub \
    -t 'test/docker/cmd/replay' -m '{}'

# 观察历史数据回放到 test/docker/history 主题
docker exec ic_test_mqtt mosquitto_sub \
    -t 'test/docker/history' -v

# 触发时间段回放（不改断点）
docker exec ic_test_mqtt mosquitto_pub \
    -t 'test/docker/cmd/replay' \
    -m '{"start":"2024-01-01T00:00:00Z","end":"2024-01-02T00:00:00Z"}'
```

### 7. SSE 实时推送

```bash
# 验证 SSE 流（curl 保持连接，观察数据到达）
curl -N http://127.0.0.1:15647/api/stream
# 期望每次设备数据更新（~1s）即收到一行：
# data: {"status":{...},"points":{...},"logs":[...]}
#
# 浏览器监控页打开后不再有 2s 轮询，DevTools Network 只有一条
# /api/stream 持久连接，状态为 "EventStream"

# 心跳：3 秒无数据更新时发出注释行（防代理超时断连）
# : hb
```

### 8. SQLite 批量写入

```bash
# 采集器运行中，观察 WAL 文件大小变化节奏
ls -la cache.db-wal        # 16 条缓冲满或 2s 定时提交一次

# 统计实际入库行数
sqlite3 cache.db "SELECT COUNT(*) FROM cache;"
```

### 9. Debug 前端

```bash
# 方式一：命令行参数
./build/industrial_collector test/docker/config.docker.json --debug
# 日志：[WARN] 【DEBUG】前端从磁盘实时伺服: .../web/（改完刷新即可，无需重编）

# 方式二：config 中设置
# "web": { "debug": true }

# 效果：直接修改 web/index.html 或 web/assets/app.js，
# 刷新浏览器即可看到变化，无需重新编译
```

### 10. 「生效」热切换（不重启换点表）

`--from-db` 下，Web 点「生效」后采集线程会用新点表重建，进程不重启。

```bash
# 起一个隔离实例（config.db 是相对 CWD 的，务必单独开目录，别和主实例共用）
mkdir -p /tmp/hotswap && cd /tmp/hotswap
cp /projects/91/industrial_collector/test/docker/config.docker.json config.json
# 把 web.port 改成没人用的（例：15649），cache.enabled 改 false

BIN=/projects/91/industrial_collector/build/industrial_collector
$BIN config.json &                       # 首次跑：把 config.json 迁进 config.db
curl -s -X POST localhost:15649/api/compile -d '{}'   # 编出 v1
# 停掉，改用快照启动
$BIN config.json --from-db &

# ── 换点表 ──
curl -s localhost:15649/api/tree/csv -o t.csv
#   在 t.csv 里加一行 / 改个 scale（Excel 也行）
curl -s -X POST localhost:15649/api/tree/csv --data-binary @t.csv   # ⚠️ 必须 --data-binary
curl -s -X POST localhost:15649/api/compile -d '{}'
#   → {"applied":true, "reload_msg":"已热切换到 v2：任务 6 / 测点 20"}
curl -s localhost:15649/api/points        # 新测点应已出现，进程 PID 不变
```

判据（缺一不可）：

| 现象 | 说明 |
|------|------|
| PID 不变 | 没有重启，`/proc/<pid>` 一直在 |
| 日志出现 `配置已更新，重建采集器` | 每个受影响任务一条；实测 630ms~2.6s 内全部完成 |
| `/api/points` 出现新测点、旧测点值按新 scale 变化 | 点表真换了，不只是库里变了 |
| 新增任务 → 日志 `新增任务，启动采集线程` | 不必重启就能加任务 |
| 删除任务 → 日志 `已从配置中移除，线程退出`，且实时监控里那一行消失 | 不留幽灵行 |
| 把任务全删光再「生效」→ `applied:false`，采集继续跑 | 坏快照绝不能清空运行中的配置 |

**`--data-urlencode` 会失败**：CSV 导入读的是**原始请求体**，
`--data-urlencode "csv@t.csv"` 会把内容裹成 `csv=...` 表单，服务端把它当第一行
CSV 解析，报"第 1 行列数不足"。用 `--data-binary @t.csv`。

### 11. DLT645 串口路径（虚拟串口）

`dlt_sim.py` 只监听 TCP，走的是采集器的 `TcpTransport`。而 DLT645 在现场多数是
RS485 直连，走 `SerialTransport`(termios) —— 那条路径此前**一次都没跑过**。

`pty_bridge.py` 开一个 PTY，把从设备端收到的字节原样转给容器里那个 DLT645 TCP
模拟器，回帧再写回来。**协议逻辑仍然只有 `dlt_sim.py` 一份**，没有第二个模拟器。

```bash
bash test/run_test.sh up      # 已含虚拟串口桥；down 时一并停掉
# 手动起：
python3 test/docker/simulators/pty_bridge.py --tcp 127.0.0.1:15644 --link /tmp/ic_ttyDLT645
# → [pty_bridge] /tmp/ic_ttyDLT645 → /dev/pts/4  ⇄  127.0.0.1:15644
```

`config.docker.json` 里的 `dlt645_serial` 已指向 `/tmp/ic_ttyDLT645`。

**判据不是"设备变成运行中"**，而是**同一个模拟器、同一批 DI，两种传输逐点一致**：

```bash
curl -s localhost:15647/api/points | python3 -c "
import sys,json;d=json.load(sys.stdin)
tcp={p['name']:p['value'] for p in d['dlt645_gw/meter_01']}
ser={p['name']:p['value'] for p in d['dlt645_serial/meter_01']}
print('一致' if tcp==ser else '不一致'); print(tcp); print(ser)"
```

实测五个测点值与质量全部相同，30s 内两侧轮次同为 18、0 坏点、0 错误、无重连。

#### 覆盖到什么、覆盖不到什么 ⚠️

| | |
|---|---|
| ✅ 覆盖 | `open(O_NONBLOCK)` / `tcgetattr` / `tcsetattr` / `tcflush` / `tcdrain`、poll 读超时，以及 `writeAll`、`readDeadline` 的 `socketMode=false` 分支 |
| ❌ 不覆盖 | **真实波特率**（PTY 上 `cfsetispeed` 会被接受但没有意义）、RS485 **半双工收发切换时序**、**校验/帧错误** |

所以准确的说法是"termios 调用路径已覆盖"，不是"串口路径已覆盖"。真波特率与
半双工时序仍需真实 RS485 设备。

**软链接不建在 `/dev` 下**：用 `/tmp/ic_ttyDLT645`，免得日后插了真 USB 转串口
被 `/dev/ttyUSB0` 撞上，也不必往 `/dev` 里写。若想让 `config/config.json`
（示例配置，写死 `/dev/ttyUSB0`）不改就能跑：

```bash
ln -s /tmp/ic_ttyDLT645 /dev/ttyUSB0     # 需要 root，自行决定
```

**桥的三个坑**（都会看着像采集器有毛病）：

- 桥**全程扣着从设备端 fd 不关**。一关，采集器断开的瞬间 PTY 就被拆掉、软链接
  悬空；而且主设备端 `read()` 在无从设备打开时会不停返回 `EIO`，`select` 立刻
  可读，循环空转烧 CPU。
- 桥启动时就把从设备端设成 **raw**。采集器自己在 `open()` 里也设，但那是它连上
  之后的事；在此之前 PTY 默认带回显，先到的字节会被弹回去，采集器随后把自己
  发出的请求当成应答帧读进来，症状是**校验和莫名其妙对不上**。
- 停桥**只按 pid 文件停**，不用 `pkill -f pty_bridge` —— 发起命令的 shell 自己的
  命令行里就含着这个模式，会把自己一起打中（见本文前面那条同类警告）。

### 12. 储能站大规模点表（设备规格驱动 / P4 层级聚合的数据源）

13084 个测点的储能电站，跑在独立实例 **15688** 上，与六协议互操作实例 15647 分开。

```bash
# 模拟器随 ic_sims 容器一起起（宿主机 15650）
bash test/run_test.sh up

# 建设备规格 + 设备树 + 采集任务 + 地址绑定（可重复跑，树已存在会跳过）
bash examples/ess_models.sh http://127.0.0.1:15688

# 「生效」编译，然后用专用配置启动（devices[] 是空的，必须加 --from-db）
curl -s -X POST http://127.0.0.1:15688/api/compile -d '{}'
./build/industrial_collector examples/ess.config.json --from-db
```

实测：6082 节点 / 13105 测点编译耗时 **236ms**；13084 点合并成 **162 个读请求**，
一轮 `readAll()` **69ms**（2s 周期里绰绰有余），13084/13084 GOOD。

#### 判据是层级自洽，不是"全 GOOD"

模拟器的值按拓扑算出来：**包电压 = 它那 24 个电芯之和**，包内极值 = 那 24 个的
真实 max/min，簇电压 = 它那 4 个包之和。所以聚合有标准答案可核对：

```bash
curl -s localhost:15688/api/points | python3 -c "
import sys,json
by={p['name']:p['value'] for p in json.load(sys.stdin)['bms_modbus']}
pk='station.rack01.cluster1.pack1'
cells=[by[f'{pk}.cell{i:02d}/voltage'] for i in range(1,25)]
print('24 电芯之和',round(sum(cells),3),' pack_voltage',by[f'{pk}/pack_voltage'])
print('电芯 max/min',max(cells),min(cells),' 上报',by[f'{pk}/cell_v_max'],by[f'{pk}/cell_v_min'])"
```

实测差 **2mV**（uint16 在 scale 0.01 下的量化误差），簇级差 0.05V。
填随机数的话，聚合算错了根本看不出来 —— 这是这个模拟器存在的理由。

顺带要看的两个量纲判据（都是"取默认值也不崩、只会悄悄算错"的地方）：

| 检查 | 正确 | 出错时的样子 |
|---|---|---|
| 电芯电压 | 3.2 V 左右 | **3200**（scale 丢了） |
| `pack_current` | 有正有负（充放电） | 全是 **6 万多的正数**（int16 被当 uint16） |

#### 与 `modbus_sim.py` 的分工

两个都用 pymodbus（独立于采集器的 libmodbus），**都算真互操作**。区别在用途：

| | 点数 | 验什么 |
|---|---|---|
| `modbus_sim.py`（15641） | 3 | 协议正确性 |
| `ess_modbus_sim.py`（15650） | 13084 | 规模、分组合并、层级聚合 |

#### 地址映射两处必须对齐 ⚠️

`examples/ess_models.sh` 的绑定公式与 `ess_modbus_sim.py` 的基址常量是**两份**
需要手工保持一致的东西。改一边不改另一边，症状是**连接正常但点全 BAD**。

```
CELL  电压 10000 + gidx*2      温度 40000 + gidx
PACK  46000 + gidx*8 + k       CLST 48000 + gidx*8 + k
RACK001 49000 + gidx*8 + k     RACK002 49100 + gidx*8 + k
CTL   49200 + gidx*16 + k      BOOST 49300 + k
```

用 `gidx_<MODEL>`（全局实例序号）而不是 `idx_<MODEL>`（同父下的序号）：地址在整
个电站里必须唯一，而 `idx` 每换一个父节点就从 0 重来。RACK001 与 RACK002 是两个
**独立**的 gidx 计数器，所以各占一段基址。

#### 已知：MQTT 发布侧扛不住 ⚠️

每轮 13084 条逐点发布会把 paho 缓冲区打满，约 1/3 被丢：

```
MQTT 发布失败 4424 条（最近 …/cell_v_min: MQTT error [-12]: No more messages can be buffered）
```

**采集侧毫无压力，瓶颈完全在发布侧**，留给 P3 的通道化解决（批量成帧 + 背压）。
日志已按秒聚合，否则一个周期近万行会把轮转日志和 Web 日志环一起冲光。

### 13. 多通道并行（MQTT + 文件）

启动读 `channels` 表；表空则从 `config.json.mqtt` 造一个 id="mqtt-default" 的
默认通道（向后兼容，现场升级不断）。要真正的多通道，往表里加：

```bash
curl -sX POST localhost:15688/api/channels -H 'Content-Type: application/json' -d '{
  "id":"file-archive","type":"file","direction":"up","enabled":true,
  "config":{"dir":"/tmp/ess_archive","rotate_mb":10,"keep":3}
}'
curl -sX POST localhost:15688/api/channels -H 'Content-Type: application/json' -d '{
  "id":"mqtt-primary","type":"mqtt","direction":"up","enabled":true,
  "config":{"broker":"127.0.0.1","port":15640,"client_id":"ic_ess_ch_mqtt",
             "topic_prefix":"ess/station","qos":0,"retain":false}
}'
# 通道更新目前需要重启才生效（P3.2b 加热切换）
./build/industrial_collector examples/ess.config.json --from-db
```

判据 —— 两个通道的输出都得能重装并通过整站层级一致性：

```bash
# 文件侧：每行一整轮
python3 -c "
import json;j=json.loads(open('/tmp/ess_archive/bms_modbus.jsonl').readlines()[-1])
by={k:v['value'] for k,v in j['points'].items()}
pk='station.rack01.cluster1.pack1'
cells=[by[f'{pk}.cell{i:02d}/voltage'] for i in range(1,25)]
print(f'24 电芯之和 {sum(cells):.3f}  pack_voltage {by[f\"{pk}/pack_voltage\"]:.3f}')"
# MQTT 侧：见第 12 节的 mosquitto_sub + 按 timestamp 分桶重装
```

实测 32 轮 × 13084 点，双通道并行**、0 错误**、两侧 240 包 + 60 簇的层级
自洽全部通过。

#### FileChannel 明确不做的事

不 fsync（flush 到 kernel 已够，掉电最多丢最后一行；每行 fsync 会把 SD 卡打
成砖头）、不压缩（`jq` / `grep` 用起来省事）、不保证文件切换期间的原子性。
`.jsonl` 一行一整轮，下游按 `\n` 切读即可。

#### 通道 `direction`

- `up` / `both` 参与上行发布
- `down` 目前跳过（下行任务级启停留给 P3.2b）
- 不识别的 `type`（例如 `kafka`）会告警跳过，其余通道继续跑 —— 单条错配不影
  响整机

### 单元测试的参数约定 ⚠️

九个测试可执行文件不是同一套参数，传错了会得到看着像 bug 的失败：

```bash
cd build && make config_db_test runtime_test compile_test formula_test \
                 dlt_frame_test can_signal_test io_timeout_test \
                 dlt_deadline_test data_cache_test
cd ..

# 收 config.json 的：argv[1]=配置文件, argv[2]=临时库目录
./build/config_db_test config/config.json /tmp/t
./build/runtime_test   config/config.json /tmp/t

# 只收目录的：argv[1]=临时库目录
./build/compile_test    /tmp/t
./build/data_cache_test /tmp/t

# 不收参数的
./build/formula_test; ./build/dlt_frame_test; ./build/can_signal_test
./build/io_timeout_test; ./build/dlt_deadline_test
```

给 `compile_test` 传了配置文件路径，它会拿它当目录去建库，报 `✗ 建库失败` ——
看起来像编译器坏了，其实只是参数位置不对。

---

## 各模拟器的测试性质

| 协议 | 模拟器 | 性质 |
|------|--------|------|
| Modbus | pymodbus | **真互操作**（≠ libmodbus，独立实现） |
| OPC UA | asyncua | **真互操作**（≠ open62541） |
| IEC104 | c104 / lib60870 | **真互操作**（≠ 本项目手写实现） |
| IEC61850 | libiec61850 server_example | **真互操作**（同库，独立 server/client 进程） |
| DLT645 | 自写 socket | ⚠️ **仅自洽**（与采集器同源） |
| DLT698 | 自写 socket | ⚠️ **仅自洽** |

DLT 模拟器与采集器出自同一理解，跑通只证明自洽。**规约符合性**由字节级单元测试（见第2节）保证。

Docker 里的 DLT 模拟器只监听 TCP。串口那条路由 `pty_bridge.py` 补上（见下节），
但**只到 termios 调用这一层**，不等于"串口路径已覆盖"。

---

## 协议默认端口模板

`test/docker/config.docker.json.example` 使用协议标准默认端口（502 / 2404 / 102 / 4840 / 1883）
和串口默认参数，可作为接**真实设备**时的起点配置。

---

## 常见问题

| 现象 | 原因 | 解决 |
|------|------|------|
| `Connection refused` | config 端口与 compose 宿主端口不一致 | 检查 DLT 用的是 `tcp_port` 不是 `port` |
| `docker compose` 报错 unknown flag | 老版本 docker | 用 `docker-compose`（脚本已自动兼容） |
| OPC UA 连上又断 | `opcua_sim.py` 宣告端口与映射端口不一致 | 三处同步：sim.py / compose / config |
| IEC61850 值恒为 0 | `object_ref` 路径写错 | LD 名是 `simpleIOGenericIO` 不是 `GenericIO` |
| IEC61850 服务端起不来 | 102 端口需 root | 改用非特权端口如 `15646` |
| HTTPS `curl` 失败 | 证书路径不存在 | 先运行 `bash scripts/gen_cert.sh` |
| 配置保存后设备不变 | 设备连接参数需重启 | 重启采集器；MQTT/缓存参数立即生效 |
| `sizeof(bool)==1` 编译报错 | libiec61850 MSVC shim stdbool.h 未删除 | bootstrap.sh 已自动删除，重跑 bootstrap |
