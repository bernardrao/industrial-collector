# 测试环境（Docker）

独立的协议模拟器 + MQTT broker，用于端到端测试 `industrial_collector`，
**不污染主机**：所有模拟器跑在容器内，仅通过 `127.0.0.1` 发布端口暴露。

## 架构

```
宿主机                              Docker 容器
┌───────────────────────┐          ┌─────────────────────────────┐
│ industrial_collector  │   MQTT   │ mqtt (eclipse-mosquitto)     │ :1883
│ (已编译的二进制)        │ ────────▶│                             │
│                        │          └─────────────────────────────┘
│ config.docker.json     │          ┌─────────────────────────────┐
│   → 127.0.0.1:端口     │ ◀──采集──▶│ simulators (python)         │
│                        │          │   Modbus   :1502            │
│ iec61850_server        │ ◀──采集──▶│   IEC104   :2404            │
│ (宿主机本地，:15646)   │          │   OPC UA   :15643           │
└───────────────────────┘          │   DLT645   :20108           │
                                    │   DLT698   :20109           │
                                    └─────────────────────────────┘
```

采集器本身**不进容器**（避免把 bootstrap 依赖链打进镜像），
跑在宿主机连接发布端口。IEC61850 服务端也跑在宿主机（纯 C 程序，直接编译）。

## 端口映射

| 服务 | 宿主端口 | 容器内端口 |
|------|---------|-----------|
| MQTT | 15640 | 1883 |
| Modbus | 15641 | 1502 |
| IEC104 | 15642 | 2404 |
| OPC UA | 15643 | 15643（内外同端口） |
| DLT645 | 15644 | 20108 |
| DLT698 | 15645 | 20109 |
| IEC61850（宿主直跑） | 15646 | — |
| Web 界面 | 15647 | — |

## 用法

```bash
# 推荐：一键脚本（从项目根目录）
bash test/run_test.sh all

# 或手动：
# 1. 起模拟器 + broker
cd test/docker && docker compose up -d --build

# 2. 起 IEC61850 服务端（先编译）
bash ../iec61850_server/build.sh
../iec61850_server/iec61850_server 15646 &

# 3. 回项目根，用测试配置启动采集器
cd ../..
./build/industrial_collector test/docker/config.docker.json

# 4. 另开终端订阅 MQTT
docker exec ic_test_mqtt mosquitto_sub -t 'test/docker/#' -v

# 5. 浏览器看 Web 界面（AdminLTE 风格，含配置管理页）
#    http://127.0.0.1:15647

# 6. 收尾
docker compose -f test/docker/docker-compose.yml down
pkill -f "iec61850_server 15646"
```

## 各模拟器的测试性质

| 模拟器 | 实现 | 测试性质 |
|--------|------|---------|
| Modbus | pymodbus | **真互操作**（pymodbus ≠ libmodbus） |
| OPC UA | asyncua  | **真互操作**（asyncua ≠ open62541） |
| IEC104 | c104     | **真互操作**（c104/lib60870 ≠ 本项目） |
| IEC61850 | libiec61850 server_example | **真互操作**（独立 server/client 进程） |
| DLT645 | 自写 socket | ⚠️ **仅自洽** |
| DLT698 | 自写 socket | ⚠️ **仅自洽** |

DLT 规约符合性由 `test/dlt_frame_test.cpp` 字节级单元测试独立保证：
```bash
cd build && make dlt_frame_test && ./dlt_frame_test
```

详细测试说明见 `test/TESTING.md`。
