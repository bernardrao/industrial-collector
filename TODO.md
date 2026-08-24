# TODO / 待办

> v1.4.0 四项功能 2026-06-02 全部交付；v1.5.0 优化项 2026-06-03 完成。
> v1.6.0 CAN / CAN FD 2026-07-09 完成；同日修复十二项稳健性/安全缺陷（见下）。
> v2.0.0 架构升级 2026-08-17 启动：模型驱动（设备规格 / 设备树 / 采集任务 / 通道）。

## v2.0.0 架构升级（2026-08-17 起）🚧

从"扁平设备列表"转为"模型驱动"。六大板块：实时监控 / 采集设置 / 设备组装 /
通讯通道 / 系统设置 / 系统日志。

**核心模型**（依赖单向）：

```
设备规格(模板) ──实例化──▶ 设备树(实物) ──编译──▶ 点表 ──▶ 采集任务(线程)
                                                          │
                                        通道(MQTT/Kafka/文件) ◀─┘ 数据上行
                                        通道 ──指令下行──▶ 任务
```

**已定的设计决策**（2026-08-17 与用户确认）：

| 决策 | 选择 | 理由 |
|------|------|------|
| 配置存储 | SQLite `config.db`，与 `cache.db` 严格分开 | 配置低频写强事务、跟随部署走；缓存高频写 WAL、可随时删除重建 |
| 存储职责边界 | **系统设置留 `config.json`，采集配置入 `config.db`** | Web 端口/鉴权/日志/缓存少量且需人工可编辑 —— 鉴权配错进不去 Web 时得能用文本编辑器改回来；设备规格/设备树/测点是万级批量数据，正是 SQLite 该管的 |
| 组合表示 | `model_children` 结构化存储 | `PACK001.CELL001024` 这类编码进名字的写法仅作显示；靠解析字符串还原结构在型号名含数字或数量超 999 时会出歧义 |
| 节点身份 | `id` 代理主键，`path` 派生列 | 若以 path 为身份，重命名一个舱要重写数千后代行及其全部引用；代理键让重命名只影响子树 path |
| 公式变量 | 各级祖先序号 `idx_<模型code>` | 电芯地址同时取决于它在包内的序号和包在簇内的序号，只有 sibling_id 不够 |
| 下行控制 | 先任务级启停，`write_point` 留接口 | 协议写路径需逐协议加权限控制，单独作为 P3.5 |
| 旧配置 | 一次性自动迁移，原文件留档 | 避免长期双格式兼容层 |
| Kafka | librdkafka 静态编译 | 事实标准、纯 C、ARM 交叉编译友好 |

### P1a：配置库 schema + 迁移器 ✅（2026-08-17）

| 交付 | 说明 |
|------|------|
| `include/config_db.h` / `src/config_db.cpp` | 12 张表；`PRAGMA foreign_keys=ON`；RAII 语句句柄；组合环检测 |
| `src/config_migrate.cpp` | `config.json` ⇄ `config.db` 双向，JSON 层面工作 |
| `test/config_db_test.cpp` | 语义往返 + 外键 + 环检测 + path 派生 |

**无损是构造性的**：点位除抽取可查询列外整条原样存 `raw_json`，新增协议字段
无需改迁移器，也就不存在"忘了搬某个字段"。逐字段映射正是迁移器静默丢字段的根源。

**三处经对照组/实测验证的判断**：

1. *往返测试非空*——注入"少存 unit 字段"故障后，测试抓出全部 21 处丢失并逐条
   指出 JSON 路径（覆盖 7 协议 4 种容器形状）。第一次就全绿的测试必须自证有效。
2. *节点排序必须按 `(parent_id, ord)` 而非 `path`*——对照组用 12 块表配置证明
   按 path 排序时 `meter_2` 被 `meter_10` 顶掉，22 处错位。
3. *容器键必须由协议决定，不能靠"有没有子节点"反推*——实测 `meters: []` 的空
   总线会丢 `meters` 键并凭空多出 `points` 键。P1b 的 Web UI 必然造出这种中间
   状态（建了总线还没添表），已加为常驻用例。

**单一权威副本**：`poll_interval` 已从 `endpoint_json` 剔除，只存 `tasks.interval_ms`，
导出时再换算回去。两处都留的话，P1b 的"改采集周期"会写 `interval_ms` 而导出读
`endpoint_json`，改动静默丢失（`enabled` / `reconnect_sec` 本就只存在于 tasks）。

**P1b 动手前需注意**：`addModelChild` 用 `ON CONFLICT(model,ord) DO UPDATE`，
CRUD 若不递增 `ord` 会静默替换既有兄弟项而非报错。RACK002（7×CLST001 + 1×CTL001）
正是会踩中的形状 —— CRUD 层要么自动分配 ord，要么显式拒绝重复 ord。

### P1b：CRUD API + CSV/JSON 导入导出 + Web 六板块框架 ✅（2026-08-17）

| 交付 | 说明 |
|------|------|
| 存储层 CRUD | 改名/移动（子树 path 重写）、`appendModelChild`（ord 自动分配）、各类删除 |
| `src/config_csv.cpp` | 设备树 ⇄ CSV，每测点一行，供 Excel 批量编辑 |
| `src/web_api_model.cpp` | 设备规格 / 设备树 / 任务 / 通道 的 REST 接口 |
| `web/assets/model.js` + 六板块导航 | 实时监控 / 采集任务 / 设备规格 / 设备树 / 通道 / 系统设置 / 系统日志 |
| 首次运行自动迁移 | 空库时从 `config.json` 迁入（实测：任务 8 / 节点 11 / 点位 21）|

**改名/移动只重写子树 path**，测点与快照的引用指向 `id` 不受影响 —— 这是选代理
主键的全部理由，已由测试固化（改名后 `id` 不变、测点仍在）。子树定位用 `substr`
而非 `LIKE`：节点名可含 `%` / `_`，那是 LIKE 通配符，不转义会误伤兄弟节点。

**CSV 导入为全量替换**：先在内存全量校验（父节点存在、点位不重名、数值可解析），
通过后才在一个事务里重建。三次坏 CSV 实测均被拒且原树 11 节点不变。

**两处对照组验证**：

1. 导出时"跳过无点位的节点"看似合理的优化，实测会让 DLT 总线节点消失、其下
   电表变成孤儿，**整个导入失败**（0 节点 0 测点）。故无测点的节点也必须占一行。
2. *CSV 往返曾损坏 `raw_json`*——原实现按 10 个 CSV 列重建 `raw_json`，实测
   **130 处字段丢失**：`unit` / `description` / `func_code` 全没，`address` 从
   整数退化为字符串，CAN 信号的 `start_bit` / `bit_length` / `byte_order` /
   `is_signed` 尽失且被塞上 `address`——已不再是 CAN 信号。
   起初没查出来是因为判据放错了位置：`csv1 == csv2` 只证明那 10 列稳定。
   改成 **JSON 两端比对**（库→CSV→库→导出）后立刻暴露。
   修法是**合并**：已存在的点位只覆盖 CSV 真正携带的字段，地址写回它原本的键
   （CAN 写 `can_id` 而非 `address`）；数值单元格与旧值格式化结果一致即视为
   未改动而保留旧值 —— 否则 `0.1f` 提升为 double 的 `0.10000000149…` 会被 CSV
   的 6 位有效数字逐次截断，用户只想改一个地址却把满表系数都改了。
   修复后 130 → **0** 处；另有专测证明"改地址/系数确实生效、同行其余字段不动"。

**MQTT 单一写入口**：MQTT 不入库，仍由 `config.json` 承载。否则系统设置页写
`config.json`、通道页写 `config.db`，用户在通道页改完 broker 看到"保存成功"
却毫无效果。P3 引入真正的多通道时再整体迁移，并同时撤掉系统设置的 MQTT 页签。

**尚未接线**：这些接口只读写 `config.db`，采集仍按 `config.json` 运行 —— 编辑
设备树不会影响现场采集。P2 的"编译/生效"就位后才切换。

**遗留小项**（不阻塞 P2）：
- `main.cpp` 里 `config.db` 路径相对 CWD（与既有 `cache.db` 一致）。systemd 若
  配了不同的 `WorkingDirectory`，会新建一个空库并静默重新迁移一次。宜改为可配置。
  **P2c 之后这条的严重性升了一级**：热切换让「生效」成了日常操作，而进程读的
  库和它写版本的库一旦不是同一个，用户会看到"点了生效、也没报错、现场就是不
  变"——比启动时多迁一次难查得多。改成配置项（或至少启动时把绝对路径打进日志）
  应在 P3 之前做掉。
- `settings` 表在职责划分调整后已无人写入，`getSetting` / `allSettings` 暂成
  死代码。留给 P2 存编译快照的元信息，勿再把 config.json 接回去。

### P1c：实例化 + 储能站完整示例 ✅（2026-08-18）

| 交付 | 说明 |
|------|------|
| `instantiateModel` / `countInstanceNodes` | 设备规格 → 设备树子树，单事务展开 |
| `POST /api/tree/instantiate` | 前端"实例化设备规格"按钮 |
| `GET /api/tree?parent=<id>` | 按需展开，替代整棵树下发 |
| `examples/ess_models.sh` | 七层储能设备规格 + 电站实例化，可重复运行 |

**实测**：7 个设备规格 → 6085 节点（5760 电芯 / 240 包 / 60 簇），**1.5 秒**建完。
路径形如 `station.rack01.cluster1.pack1.cell01`。

**先数后建**：展开是指数级的（RACK001 一台 809 节点），故 `countInstanceNodes`
先只数不建，超 `max_nodes`（默认 20000）直接拒绝 —— 建到一半再失败留下的半成品
树比彻底不建更难收拾。整个展开在一个事务内，逐条自动提交会慢上百倍。

**节点不复制模型测点**：5760 电芯 × 2 点 = 11520 行全部可由模型推导，落库纯属
冗余。测点在 P2 编译时按公式展开。

**这个规模暴露的两个真问题**：

1. *设备树整棵下发不可用*——`/api/tree` 原本返回全部节点：0.9MB、6085 行扁平
   DOM，页面直接废掉。改为默认只返回一层、前端点击展开；根层 2.5KB，比全量
   小 **357 倍**。整棵树仅在导出等场合用 `?flat=1`。
2. *CSV 导入被 httplib 卡在 8KB*——`CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH`
   默认 8192，而 `curl --data-binary` 默认就发 `application/x-www-form-urlencoded`，
   于是 330KB 的设备树 CSV 被 413 挡下，**且错误响应体为空**，毫无线索。
   本项目没有表单编码接口，该上限只会误伤 CSV 导入，已放宽到 64MB（仍有界）。
   前端走 `application/json` 不受影响，但用 curl/脚本的人必踩。

### P2a：公式求值器 + 编译器 + 「生效」✅（2026-08-18）

| 交付 | 说明 |
|------|------|
| `include/formula.h` / `src/formula.cpp` | 递归下降整数表达式求值，带位置的错误诊断 |
| `src/config_compile.cpp` | 设备树 × 设备规格绑定 → 编译快照 |
| `POST /api/compile` | 「生效」；`GET /api/compiled` 查看结果 |
| 绑定 CRUD + `POST /api/formula/eval` | 公式入库前先做语法检查，可试算 |
| `test/formula_test.cpp` / `test/compile_test.cpp` | 38 + 30 条断言 |

**实测**：真实储能站 5769 节点 → **11541 个测点，224ms**。电芯电压地址
10000..21518，5760 个，**全部唯一、步长恒为 2**。

**整数运算而非浮点**：结果要当寄存器地址用，`1999.9999` 这种取整歧义不可接受。
字面量支持 `0x` 十六进制（CAN ID 惯用写法），写小数点会明确报错而非悄悄截断。
加减乘都做溢出检查 —— 公式写错不该回绕成一个"看起来合法"的地址。

**错误必须可诊断**：公式是现场人员在 Web 上手写的。未知变量会列出当前可用的
全部变量（`idx_pack` 写错成小写是常见笔误），并用插入符指到出错字符；编译期
的错误还会带上是哪个节点的哪个测点。

#### 三个由真实规模暴露的设计问题

1. **兄弟序号必须按模型分别计数**——RACK002 = 7×CLST + 1×CTL，若按"全部兄弟"
   编号，控制器会顶偏其后所有簇的序号。对照组实测：控制器排在最前时，簇的
   基址从 1000 错位成 1512。（控制器排在最后的用例没有区分度，专门补了
   "控制器在前"的 RACK002 才测得出来。）

2. **地址唯一性的作用域随协议而变**——一刀切检查在真实电站刷出 **9990 条**告警，
   把真问题彻底淹没：
   - *CAN*：同一帧按 bit 偏移打包多个信号是常态，共享 CAN ID 恰恰是正确配置 → 不检查
   - *DLT645/698*：一根总线多块表，各有独立数据标识空间，两块表都有
     `data_id=00010000` 完全正常 → 按"表"分别检查
   - 其余：一个连接一个地址空间 → 按任务检查

   修正后降到 **6 条**，且全部是真问题（那 6 个模型确实还没配绑定）。

3. **异构树需要全局序号 `gidx_<MODEL>`**——结构化地址（舱\*N + 簇\*M + …）要求
   公式能引用每一级祖先，但电芯挂在 RACK001 下时只有 `idx_RACK001`，挂在
   RACK002 下时只有 `idx_RACK002`，一条公式引用不了两者。加入"该实例在全树
   同型号中的序号"后，`10000 + gidx_CELL001*2` 就给全站 5760 个电芯排出了连续
   且唯一的地址。这是写例子时真撞上的墙，不是预设的功能。

**告警上限**：`POST /api/compile` 只回前 50 条并给出总数 —— 一处公式写错会让
上万个实例各报一次，全量下发既撑爆响应体也淹没信息。

### P2b：采集接入编译快照 ✅（2026-08-18）

| 交付 | 说明 |
|------|------|
| `src/config_runtime.cpp` | 编译快照 → `devices[]` 形状，交既有解析器 |
| `--from-db` 启动开关 | 默认仍读 `config.json`，显式加参数才走快照 |
| 设备树页「生效」按钮 | 编译并回显统计与告警 |
| `test/runtime_test.cpp` | 逐条核对地址、测点数、DLT 两级结构 |

**七个采集器一行都没改。** 还原成 `devices[]` 再喂给既有的
`appConfigFromJsonString`，风险因此集中在一个可离线验证的转换函数里，而不是
散进现场跑了很久的采集代码。实测六协议全部 `运行中`，数值与旧路径一致。

**默认不切换**：`--from-db` 是显式开关，不是过渡期的犹豫 —— 现场升级时应能
一条命令切回旧行为。`mqtt` / `web` / `logging` 始终来自 `config.json`，不受影响。

**测点命名的兼容性**：同一个 `point_key` 现在会出现在 5760 个电芯上，故运行期
名字取"节点路径/测点键"。但迁移来的老设备（节点路径 == 任务 id）保持原名 ——
否则升级后 MQTT 主题全变，下游订阅全断。已由测试钉死。

**编译器对禁用任务也编译**，由运行时按 `tasks.enabled` 过滤 —— 这样启用一个
任务不必重新「生效」。（写测试时先按"全部编译=全部还原"断言，实测 21 vs 13
才厘清这个语义。）

#### 又一个被真实使用暴露的坑

**httplib 拒绝没有请求体的 POST**：`POST /api/compile` 无参，curl 不带 `-d` 时
直接回 **400 且响应体为空**，处理函数根本不会被调用 —— 前端表现就是"点了没
反应，也没有任何错误提示"。前端 `api()` 已改为 POST/PUT 一律至少发 `{}`。
（这个 400 我一开始误判成告警 payload 太大，实际与大小无关。）

### P2c：「生效」热切换 ✅（2026-08-19）

点「生效」后不重启进程即换点表。

| 交付 | 说明 |
|------|------|
| `SharedState.reload_seq` / `runtime_version` | 配置世代号；采集线程每轮比对 |
| `ReloadFn` 回调（`config_db.h`） | Web 层编译成功后回调 main 换表，`/api/compile` 返回 `applied` + `reload_msg` |
| `TaskThreads` 线程池 | 「生效」新增的任务当场起线程，删除的任务线程自行退出 |
| `SharedState::removeDevice` | 任务消失时抹掉它的统计与实时数据 |

**不给七个采集器各加一套"运行中换点表"的逻辑**，而是复用已经跑熟的重连通路：
世代号一变，采集循环结束本次连接返回，外层 `runDeviceForever` 用新配置重建。
代价是一次亚秒级重连 —— 实测 630ms~2.6s（上界是各设备自己的轮询周期）。

**失败一律不动运行中的配置**。快照构不出或解析不了时（例：任务全删光，
`buildRuntimeJson` 返回"没有任何可运行的任务"），`state.config` 原样保留、
`reload_seq` 不递增、采集继续跑，前端如实显示"热切换失败（运行中的配置未改动）"。
版本此时已落库，重启即可生效 —— 所以编译成功与换表成功必须分开回报，
笼统报一句"生效成功"会让用户以为现场已经在跑新点表了。

**没加 `--from-db` 就不热切换**：用户明确选了 `config.json` 作采集源，不能因为
他点了「生效」就悄悄切到库上。此时回一句"重启时加 `--from-db` 方可生效"。

#### 三个只有摆出来才会发现的并发缺口

**1. `state.config` 与采集源不同步（会让 `--from-db` 完全跑不起来）。**
`state.config = cfg` 原本在 `--from-db` 替换 `cfg.devices` **之前**执行。改成按
任务 id 现取配置之后，每个线程都会在 `state.config` 里查无此任务、当场退出 ——
进程活着、Web 正常、一个点都不采。已把 `state.config = cfg`、`initDeviceStats`
和设备计数横幅一并移到采集源确定之后。

迁移来的库与 `config.json` 任务 id 相同，所以**拿现成的测试实例是测不出来的** ——
两边 id 集合重合，查得到就不报错。判据必须打破这个对称：建一个**只存在于
`config.db`** 的任务（`db_only_task`），
加 `--from-db` 启动它必须在采集（实测 `运行中`，7 轮 0 坏点、日志无一条
"已从配置中移除"），不加则必须查无此任务。这一对才钉得住加载顺序。

**2. 世代基准取晚了，等于留了个几秒宽的丢更新窗口。**
最初三个采集循环各自在进入循环时才读 `reload_seq`，而 DLT 两个总线是在
`transport->open()` **之后**才读的 —— TCP 握手/串口打开要花上几秒，落在这个
窗口里的「生效」会被整个吞掉，该设备一直用旧点表跑到下一次「生效」。
现改为 `runDeviceForever` 在**同一把 `config_mutex`** 里成对读出 `(entry, seq0)`
再传下去；写侧也是持同一把锁换表后、放锁前递增，两边配对。
手点一次几乎撞不到这个窗口，靠测试是钉不住的，只能靠结构排除。

**3. 删掉的任务会以一行空白幽灵重新出现在实时监控里。**
「生效」时清理 `device_stats`，与那个正在退出的采集线程的最后一次写入是并发的；
线程退出前调 `setStatus(id,"已移除")`，而 `device_stats[id]` 会**默认构造**一行，
于是刚清掉的行又被建了回来（协议列空白、状态"已移除"）。改由退出中的线程自己
调 `removeDevice()` 作最后一笔操作，顺序才是确定的 —— 「生效」时那次统一清理
随即成了摆设（清完就被各线程写回来），已整个删掉，不留两条含义重叠的清理路径。

**4. 电表这一级没人管（随 P2c 一起出现的新缺口）。**
`removeDevice` 是任务级的，看不见 `任务/表` 这一层。热切换之前电表在运行期不会
变，所以从来不是问题；现在现场删掉一块表并「生效」，那块表会**永远**挂在实时
监控上、数值定格在最后一次采集，看起来像还在跑。补 `retainBusMeters()`，在
DLT 总线重建完 collectors 后调用 —— 那时新表集已定，且本线程正是唯一会写这些
键的人，不存在"清完又被写回来"。

#### 顺带修掉的真 bug：Excel 新增点位落错键、落错类型

`POST /api/tree/csv` 对**全新**点位一律写 `{"address": "<单元格文本>"}`，两处都错：

- **键错** —— CAN 信号被写成 `address` 而不是 `can_id`，DLT 测点被写成 `address`
  而不是 `data_id`/`oad`，协议解析器根本不认，运行时表现为该点凭空消失；
- **类型错** —— Modbus/IEC104 的地址是整数，写成字符串会让整份快照在
  `appConfigFromJsonString` 抛 `type_error.302`，**一个新点位废掉整次「生效」**。

已有点位靠"从旧 `raw_json` 里认出它实际用的键"来回填（P1b 的老教训），全新点位
没有旧值可认，只能按**任务的协议**决定键名与类型 —— 见 `newPointRawJson()`。
IEC104 的 `type_id` 解析器用 `at()` 取、缺了会抛，故在导入时就挡下并报清楚。
任务还没建立时（先导树后建任务是合理次序）退回通用形状，这类点位不在 `tasks`
表里、编译快照里也就没有它，不会污染运行时。

判据落在**全链路**上：CSV 加点 → 编译 → `buildRuntimeJson` → 运行时解析器能吃下
（原 bug 正是在最后一步抛 302）。对照组同时钉住旧实现确实复现这两个错。

> 这是 Excel 批量加点的主路径，而它是靠**拿真实模拟器跑一遍**暴露的 ——
> 单测全绿，因为当时没有一条用例走"新增点位"这条分支。

### 储能站示例：从"能编辑"到"有数据" ✅（2026-08-19）

`examples/ess_models.sh` 此前只建规格和设备树，**没建任务、没建绑定** —— 跑完看着
一切正常，一点「生效」才发现"尚无编译版本"。编译器要知道每个测点归哪个任务、
地址怎么算，缺了绑定这棵树就是编译不出东西的。已补齐：

| 交付 | 说明 |
|------|------|
| `ess_models.sh` 新增 3/5 步 | 建 `bms_modbus` 任务 + 六个规格共 31 条地址绑定 |
| `test/docker/simulators/ess_modbus_sim.py` | 储能站 Modbus slave（容器内 1503，宿主 15650） |
| `examples/ess.config.json` | 该实例专用配置，`devices[]` 留空、必须配 `--from-db` |
| 脚本可重复跑 | 设备树已存在就跳过实例化，改绑定重编不会再长出一个电站 |

编译结果：**6082 节点 / 13105 测点，0 告警，236ms**。地址 10000..49307 全部唯一。

**模拟器的值按拓扑算，不是随机数**：包电压 = 它那 24 个电芯之和，包内极值 = 那
24 个的真实 max/min，簇电压 = 它那 4 个包之和。拓扑从全局序号反推
（`pack = cell//24`，`cluster = pack//4`），不必知道树本身。这样 P4 的层级聚合
就有了**可核对的标准答案** —— 界面上包电压若不等于 24 个电芯之和，那是聚合算错
了。填随机数的话，聚合写错了根本看不出来。实测差 2mV（uint16 在 scale 0.01 下
的量化误差），簇级差 0.05V。

按需重算还必须**节流**（`LiveBlock.REFRESH_SEC`）：采集器一轮打 162 个读请求，
每个请求都重算的话，同一轮里先后读到的寄存器来自不同时刻，包电压就对不上它那
24 个电芯了 —— 标准答案自己先失效。

#### 真 bug：设备规格驱动的点位丢 scale 与 data_type

`buildRuntimeJson` 只把 `addr` 当权威列写回点位 JSON，**scale / offset / dtype
没写**。迁移来的老点位 raw_json 里本就有这些字段，所以一直没暴露；而设备规格
驱动的点位，raw_json 是编译器现造的，只有 `name/address/_formula/_node` ——
scale 与 dtype 只存在于 `compiled_points` 的**列**里。

不写回去，解析器就取默认值 `scale=1` / `data_type=uint16`。症状极其隐蔽：

- 13084 个点**全部 GOOD**，实时监控一片绿，没有任何告警；
- 电芯电压显示 **3307** 而不是 3.307；
- `int16` 的负电流按 uint16 解，**-40A 变成 65496**。

修在 `applyCompiledFields()`：与地址同样按"编译列是权威"处理，dtype 按协议落到
`data_type` / `type_id` / `fc`。`runtime_test` 补了设备规格驱动的用例，对照组钉住
"编译器的 raw_json 里确实没有 scale/data_type"，判据落在 `int16` 上 —— 那是唯一
一个"取默认值也不会崩、只会悄悄算错"的地方。

> 这个 bug 从 P2b 起就在，之前所有验证用的都是迁移来的配置，**测不出来**。
> 是这次真把设备规格跑起来才现形的。

#### 副产物：MQTT 失败日志按秒聚合

13084 个点逐条发布，broker 慢一点就整轮失败，**一个周期写出近万行错误日志** ——
轮转日志几秒填满，Web 那 500 条的日志环也被冲光，真正有用的信息全被挤掉。
改成首条即时报、其余按秒聚合（`MqttPublisher::noteFailure`），实测从 9700 行/周期
降到 5 行，且汇总行保留最后一条的主题与原因。

#### 给 P3 的实测输入 ⚠️

同一次实跑暴露了通道层的真实上限，P3 必须解决：

```
MQTT 发布失败 4424 条（最近 …/cell_v_min: MQTT error [-12]: No more messages can be buffered）
```

**每轮 13084 条逐点发布，paho 缓冲区溢出，约 1/3 被丢弃。** 采集侧毫无压力
（162 个合并读请求、一轮 **69ms**，2s 周期里 CPU 基本闲着），瓶颈完全在发布侧。
P3 的通道化要一并处理：批量成帧、背压、以及"丢了要让人知道"。
现有 `publish_mode: array` 只对 config.json 里手配的 `ranges` 生效，
设备规格驱动的点位走不到那条路。

### P3.1：修 MQTT 上行 drop（先修 bug、再抽通道）✅（2026-08-19）

TODO 里 P3 的排序是 MQTT 重构 / Kafka / 文件 / 下行控制，先做通道抽象再动实现。
但实测有一个已经在发生的丢包（13084 点 1/3 被丢），而通道抽象还不存在 ——
先把七个采集器的调用点按未知需求抽出去，等抽完才知道 IChannel 该长什么样。
故把顺序改成"先修 bug，让 bug 的形状告诉 IChannel 该表达什么"。

窗口关到一半时至少 15688 还能跑，而不是七个调用点半迁移状态。

| 交付 | 说明 |
|------|------|
| paho 客户端缓冲 8192 | 默认 0，一断线立刻 [-3]；不是修，是把瞬间抖动扛过去 |
| `publish()` 短重试 | [-12] 队列满时 sleep 20ms 再试一次；持久压力再退化为聚合日志 |
| `publishPoints` 分片 | 点数 > 100 时切成 200 点/片，`prefix/batch/000..NNN` |
| 全轮共用一个 `timestamp` | 分片方案的语义关键点，见下 |

**实测**：ESS 站 13084 点 × 56 轮 = 732704 点，**0 errors、0 drops**，
一轮 67 个分片主题稳定成型。

#### 判据不是"错误归零"，是端到端能重装

只看采集器日志"errors=0"会漏掉一整类 bug：分片间时序错乱、时间戳不共享的话，
下游按 timestamp 分桶就永远拼不出完整一轮，看到的永远是"上一轮的 X 片 + 这一轮
的 Y 片"，包电压之和 = 24 电芯**永远算不对** —— 与丢包一样是"绿着算错"。

正判据：`mosquitto_sub` 抓 `ess/station/bms_modbus/batch/#`，按 timestamp 分桶
重装，然后对**整站所有** 240 个包 + 60 个簇跑一致性检查：

```
共 240 个包... 最大偏差 8.0 mV （阈值 50 mV）  失败 0 个 → ✓
共 60 个簇... 最大偏差 0.050 V （阈值 0.3 V）  失败 0 个 → ✓
```

#### 摆出来才发现的坑：分片各算各的时间戳

第一版 `buildBatchJson` 各自调 `nowIso()`，同一轮的分片时间戳互不相同。
"errors=0 但下游拼不出一轮" 是最难查的形式 —— 采集器日志、Web 界面、
`/api/points` 三处全绿。改成整轮共用一个 ts 后立即好。

#### 阈值 100 与 200 的取舍

- 小设备（Modbus 3 点 / DLT 5 点 / 常见 SCADA 站几十点）继续逐点发，
  按 `prefix/point_name` 订阅的既有下游一行不改。
- 大规格设备走分片，只多出 `prefix/batch/000..NNN` 一组主题。
- 200 点/片：JSON 序列化后 <32KB，稳在 broker 的默认最大消息限内。

#### 与 `publish_mode: array` 的关系

原先 `array` 模式只对 config.json 里手配的 `ranges` 生效，设备规格驱动的点位
走不到那条路。这次不复用它 —— 语义是"整片值 + 单个 scale"，模型驱动的分片
是"整片 JSON 对象"，两条路各自成立更清楚。

#### 未做，留给 P3.2

- IChannel 抽象：现在知道它至少要表达"批量成帧 + 分片编号 + 全轮 ts"，
  可以在 MQTT/Kafka/文件三份实现里保持一致。
- 真正的完成回调背压：paho async 的 delivery_complete 计数没接进来。当前
  的短重试足够消化瞬时抖动，但采集速率若长期超过发布速率还会堆积。
  留到 IChannel 抽象里统一做，别在 MQTT 里搭一半。
- 完成通道抽象前，`config.json` MQTT 的写入口不动 —— TODO 96-98 行已定，
  等真的多通道落地再整体迁移。

### P3.2a：IChannel 抽象 + FileChannel + 通道表接线 ✅（2026-08-19）

P3.2 原本一口气包含 IChannel / Kafka / 文件 / 下行控制 —— 拆成两步。
先抽象 + 简单实现 + 接通道表，跑起来验证抽象站得住脚；再进 P3.2b 加 Kafka
（librdkafka 静态编译是独立的构建工作）和下行任务级启停。

| 交付 | 说明 |
|------|------|
| `include/channel.h` | IChannel + ChannelBus 接口 |
| `MqttChannel`  | 组合 MqttPublisher（P3.1 的分片逻辑照搬） |
| `FileChannel` | JSON-Lines，每行一整轮，可按 MB 轮转 |
| `main.cpp` 重构 | 采集器不再看 `state.config.mqtt.qos/retain/prefix`，改调 `bus.publishRound(eid, pts)` |
| 通道表接线 | 启动读 channels 表；表空则从 config.json.mqtt 造一个默认 MQTT 通道 |

**实测**：ESS 站 32 轮 × 13084 = 418688 点，**MQTT + 文件双通道并行、0 错误**，
两侧数据都能重装并通过整站层级一致性核验（240 包 / 60 簇）。九个单测全绿。

#### 三条来自 P3.1 的硬约束进了接口

`IChannel::publishRound(device_id, points, ts)` 是唯一的上行方法：

- **一整轮 + 一个 ts** 传下去 —— 分片是通道**内部**决定；再没有"两片各算
  nowIso"的空间（P3.1 那个"绿着算错"就是这么来的，写进接口就永远不会再犯）
- 通道内可分可不分（MQTT 大规格分片，File 一行整轮）
- 不带 qos / retain / topic_prefix —— 那些是 MQTT 的概念。以前把 qos 从
  `state.config.mqtt` 里读出来沿着七个采集器往下传，现在既然通道不止 MQTT
  一种，这种耦合就守不住了

#### MqttChannel 用组合、不用继承

`MqttPublisher` 承载着连接、重连、缓存、续传、指令回调 —— 那是 P2b/P2c/P3.1
层层踩坑加固过的代码。塞进继承体系等于把它和"通道抽象"这个新东西粘死。改成
组合：`MqttChannel` 只把 IChannel 方法转发给 `MqttPublisher`。以后 MQTT 有 bug
也是原地修，不会被抽象拖累。

#### FileChannel 明确不做的事

- **不 fsync**：flush 到 kernel 已够，掉电最多丢最后一行，可以接受；每行都
  fsync 会把 SD 卡打成砖头
- **不压缩**：`jq` / `grep` 用起来省事，压缩留给外部工具
- **不保证轮转期间的原子性**：只保证已写入的**行**是完整一整行（下游按 `\n`
  切读）；文件切换那一瞬间可能丢一行，属于可接受的代价

用途：气隙 / 穿网闸场景把数据摆到磁盘让别的进程摆渡；离线诊断录一段回放。

#### 通道来源的三步优先级

1. `config.db` 的 channels 表非空 → 用它（真正的多通道）
2. 空表 → 从 `config.json.mqtt` 造一个默认 MQTT 通道，id="mqtt-default"
3. 都没有 → 报错退出

第 2 条是**向后兼容**的钩子：现场升级到 v2 时，`channels` 表本来就是空的，
让用户先去 Web 上配一条才能启动会打断一切现场升级。这条默认通道有明确的 id，
之后在 Web 上编辑即可。

#### 摆出来才发现的坑：共享 config.db 让通道变成"全局"的 ⚠️

15688（ESS）和 15647（六协议）都从项目根启动，`config.db` 是 CWD 相对的
（老遗留小项），因此**共用同一个 channels 表**。在 15688 上加一条
`mqtt-primary`，15647 也会用；两个实例 client_id 一撞就在 broker 上互踢
（`[-3]: Disconnected` 反复循环）。

现象与 P2c 后的编译版本共享是同一个根因（TODO 遗留小项那条），P3.2 把它
暴露得更明显：通道现在是配置的一部分，共享通道池等于共享路由目标。

近期解法：**每实例给 `config.db` 一个显式路径**（`--config-db PATH` 或写进
`config.json`），P3.2b 一并做。测试时手工 `curl -X DELETE /api/channels/<id>`
挪走多余通道即可。

#### 已知：热切换不覆盖通道 ⚠️

改 channels 表要重启才生效。跟 P2c 之前的任务修改一样。理由一致：
`channels` 的更新语义（连接、断开、切换主机时的 in-flight 消息如何处理）
需要单独想，不该跟"改点表"混在一起。留给 P3.2b 一并处理。

#### 缓存与续传：仍绑定单个 MQTT 通道

`DataCache` 只挂在**第一个 MQTT 通道**上（找不到就挂在 `mqtt-default`）。
多路续传语义未定 —— 同一份缓存回放到两个 broker 是重复消费，去重要额外机制。
用户想指定续传通道的话，把它排到通道列表最前即可。留给 P3.2b：把缓存也
放进"通道属性"里，每个 MQTT 通道各自决定是否需要缓存。

### 实时监控数据点分页 ✅（2026-08-20）

ESS 站 13084 点原来是**每帧全塞进 SSE**（~2MB × 每 3s），浏览器再一次渲染
13084 个 grid cell。用户请求"分页" —— 但只做前端分页会把带宽问题留在原地。
两端一起改：

| 交付 | 说明 |
|------|------|
| `buildAllJson` 每设备封顶 200 点 | 形状改成 `{total, list}`；小设备不受影响，大设备只发首屏 |
| `/api/points?device=X&page=P&size=N` | 单设备分页，size 硬顶 1000 |
| 监控页页脚控件 | 每页 50/100/200/500/1000，← 上一页 / 下一页 →，"第 X / Y 页" |
| SSE 事件当"有新数据"信号 | 大设备翻到 2+ 页时借 SSE 心跳自动重拉当前页 |
| 趋势下拉限当前页内 | 否则一次塞 13084 个 `<option>`，浏览器又卡 |

**实测**：ESS 站 SSE 帧从 ~2MB 降到 34KB，减 **60×**。131 页共 13084 点，末页
84 条，越界返回空。小设备（modbus_sim 3 点、DLT 5 点）走同一接口零变化。
九个单测全绿，两实例 40 秒 0 错误。

#### 决策：SSE 里只塞首 200 条 + 分页 API 拉后续，不是分页所有

- 小设备（≤ 200 点）走 SSE 就够了 —— 保持零 UX 回归、无额外 HTTP
- 大设备的第 2 页起用 `/api/points`；SSE 帮不到（它是广播、每客户端页码不同）
- 前端识别"total > list.length" 就自动切到轮询模式，用户看不到差别

#### 更好的做法：下钻 ⚠️

分页对拓扑数据（电芯 → 包 → 簇 → 舱）是错的抽象 —— 用户想问"这个包的 24 个
电芯"，不是"第 66 页"。测点名本来就是路径（`station.rack01.cluster1.pack1.cell01/voltage`），
天然支持下钻。分页是能兜住"点太多"的普通方案，**下钻才是这种数据的对味方案**。
留给 P4（实时监控树形下钻 + 层级聚合）—— 那还得配一套层级聚合，模拟器已经
按拓扑算了数（见 P3.2 之前那节），标准答案已在。

### P4：实时监控树形下钻 + 层级聚合 ✅（2026-08-20）

分页对**拓扑数据**是错的抽象 —— 用户问的是"这个包的 24 个电芯"，不是"第
66 页"。测点名本来就是路径（`station.rack01.cluster1.pack1.cell01/voltage`），
下钻是这类数据的对味方案。

| 交付 | 说明 |
|------|------|
| `/api/tree/live?path=X` | 返回节点自有点位 + 直接子节点（每个附带自有点位快照） |
| `handleApiTreeLive` in web_server.cpp | 拉住 points_mutex 拷一份，之后按前缀在锁外过滤 |
| 监控页视图切换 平铺 / 树形 | 有拓扑（点位名含 `.`）的设备自动进树形；用户手动切一次后锁定选择 |
| 面包屑 + 子节点行 + 聚合核对行 | 一屏内看得到本层自报值 vs 从子节点算出来的值 |
| SSE 心跳当"有新数据"信号 | 树形视图 SSE 事件到就 refresh 当前节点，节奏 = 现场更新 |
| `AGG_RULES` 五条固定规则 | pack_voltage=Σvoltage / cell_v_max=max(voltage) / … 差超阈值飘红 |

**实测**：ESS 站从 station 一路下钻到 24 电芯，**整站 240 个包全部通过层级
核验** —— `Σcells vs pack_voltage` 最大偏差 8mV（阈值 50），`max(cells) vs
cell_v_max` 最大偏差 0mV（阈值 2）。九单测全绿，两实例 40s 0 错误。

#### 决策：聚合放前端，不放服务端

- 服务端只出结构 + 点位快照（`/api/tree/live` 返回的东西对所有下游都一样）
- 前端知道自己在看什么："pack_voltage vs Σcells"这条规则只在"当前节点是包"时
  显示；若挪到服务端，就要一套单位/量纲的元数据描述规则
- 规则以固定表 `AGG_RULES` 起步；每个模型的规则不同，未来在设备规格页里
  可编辑（P4 后续），眼下够用

#### 判据是"跨越 UI 的层级自洽"

服务端接口有测试：`/api/tree/live?path=pack1` 的 `own.pack_voltage` 与
`Σ(children.voltage)` 差 ≤ 50mV。240 个包全测，最大差 8mV（uint16 scale 0.01
的量化误差正好在这里）。这个判据本来就是 P3.2 之前的模拟器为它铺垫的：
`ess_modbus_sim.py` 按拓扑算数（`pack_voltage = Σ 24 cells`），前端能看出算
错，是因为整个数据链路都有"标准答案"。

#### 未做，留给 P4 后续

- **子节点行只显示前 6 个点**：更多的点被折叠成 `+N`。够看不够点开 —— 需要
  一个"展开更多"按钮
- **聚合规则写死**：眼下写死了 5 条（pack 层 4 条 + cluster 层 1 条）。真该
  从设备规格里出，让用户自定义。现在的 UX 是"能看出算错，但看不出为什么错"
- **搜索/收藏**：13084 点里找某个电芯，靠一路点树太麻烦
- **趋势联动**：点了某个电芯的电压，趋势图应自动跟随。当前趋势下拉还只有
  平铺视图给的名字池

### P3.2b：下行控制 + 通道热切换 ✅（2026-08-21）

原本 P3.2b 三块（Kafka / 下行 / 热切换），librdkafka **本机没有**、也没有
Kafka broker 在测试环境，静态构建 + 集成是独立一整段。这次先做下行 + 热切换
两件（都是纯服务端 + 前端小改），Kafka 拆到 P3.2c。这样每一步都可交付。

| 交付 | 说明 |
|------|------|
| `MqttPublisher::subscribeCmd` | 追加订阅一个通配符主题，命中的消息转给回调 |
| `MqttChannel::subscribeCmd` 透传 | 每个 MQTT 通道各自订自己前缀下的指令 |
| main 里 `onTaskCmd` | 解析 `<prefix>/cmd/task/<id>/{start,stop}` → 改 `state.config.devices[i].enabled` → 递增 reload_seq → 落库 → ack |
| 手写 `topicMatch(filter, topic)` | `+` 单层 / `#` 尾多层；不引第三方 |
| `ChannelBus::reset(new, after_start)` | mutex 保护；旧通道移出锁再销毁；主 MQTT 通道 attachCache + subscribeCmd 由 after_start 重放 |
| `WebServer::setChannelsReload` + `POST /api/channels/reload` | 通道表 CRUD 后前端调一次 |

**实测**：
- 下行 stop → 一条 ack 干净；`modbus_sim` 从列表消失。start → ack，回到"运行中"轮次归零重启。
- 热切换：add file-hs → `/api/channels/reload` → 4 秒内 2.5MB `.jsonl`，ESS 采集继续。delete → reload → 文件字节数**不再增长**（0 秒停写）。
- 九单测全绿。

#### 摆出来才发现的两个坑（都发生在这次首跑）

**1. 下行 ack 自触发环 —— 差点搞崩 broker**
我回一条 ack 到 `<prefix>/cmd/task/<id>/ack`，broker 分发回来，又匹配
`<prefix>/cmd/task/#`，被当"未知动作 ack" → 又发一条 error ack → 循环。
第一次实测**十几秒攒了几百条 error ack**。修法：`onTaskCmd` 头一句拒绝
以 `/ack` 结尾的主题。凡自触发都源于"我发的东西又被我订上"，写这类回环
接口时得先想清楚哪些主题是自己会写的，明确排除。

**2. 前端下钻链接一片死 —— `JSON.stringify` 的引号顶破 onclick 属性**
```js
onclick="drillTo(${JSON.stringify(path)})"
// 展开成:
onclick="drillTo("station.rack01")"    // ← 引号一嵌就废
```
外层 `"..."` 遇到内层 `"..."` 直接闭合，浏览器把 `station.rack01` 当另一个属性。
所有下钻链接**点着都没反应**（用户原话"下钻链接好像是未生效的"）。改成
单引号包 `${esc(path)}` —— 路径本来就只 `[a-z0-9._]`，安全。

#### 为什么下行"停用"要走同一条 reload_seq 通路

不复用 P2c 那套的话，就得给"通过 MQTT 下发的 enabled"和"通过 Web 改的
enabled"各造一套触发路径 —— 两条路早晚会漂移，而任务重建的所有难点
（entry 与 seq0 成对读、线程正在建连接的窗口、幽灵行清理）P2c 都已经打
牢过。多加一个入口不该导致再打一遍。

#### 未做，留给 P3.2c

- **Kafka**：librdkafka 静态编译 + docker-compose 起 Kafka broker（需要
  Zookeeper 或 KRaft）+ `KafkaChannel` 实现 + 集成测试。整段。
- **write_point（点级写）**：按协议加权限控制，需要一个协议无关的"能不能
  写"的元数据模型，是另一件事。

### 实时监控左右布局 + 默认树形 ✅（2026-08-21）

P4 那一版把树形做在"面包屑 + 子节点行"上，翻十几层要一路点回上级。改成
**左右布局**：左侧一棵可任意跳的树导航（懒加载），右侧本节点自有点位 + 子节点行。
默认视图直接就是**树形**——这套系统的主用途是有拓扑的现场，扁平设备（Modbus
3 点 / DLT 5 点）在首帧收到时自动降级回平铺。

| 交付 | 说明 |
|------|------|
| `.split-view` 布局 | 260px 左树 + 1fr 右详情；<900px 窄屏栈起来 |
| `buildNav()` + `navToggle()` | 复用 `/api/tree` 的懒加载（一层一层拉），避免整棵 6000 节点一次下发 |
| `highlightNav(path)` | 点右侧或面包屑 `drillTo` 时，左树自动展开父链、高亮当前 |
| 默认 `viewMode = 'tree'` | 老代码全站默认 flat；改成 tree，DOMContentLoaded 就调好 flat-only/tree-only 显隐并 buildNav |
| `refreshTree` 遇 404 自动降级 flat | 该设备不在设备树里（共享 config.db 的另一实例的设备）时不再一屏红字 |

**实测**：ESS 站首屏左树 9 根节点，点开 `station` 后 rack/cluster/pack 逐层
展开、点电芯直接跳，右侧同步。扁平设备（modbus_sim）打开时收到 404 → 自动
切平铺 → 走原分页路径。九单测全绿，两实例 25s 稳定。

#### CSS 一处易踩：`display: none` 撕破 grid 列

`setViewMode` 里一开始把所有 `.flat-only` / `.tree-only` 都设 `style.display=''`。
`.split-view` 是 CSS Grid，两列布局；设 `display:''` 让本来 hidden 的元素回到默认
`inline`，撑不起 grid 单元格。改成显式 `display: '' / 'none'` 时确保右侧详情
容器 `.split-detail` 只把内部块切换，不动 grid 布局本身。

#### 顺带带出遗留小项的严重性

15647 打开监控页时，左树里显示的是 15688 的 `station` —— 两实例**共用**
`config.db`（老的 CWD 相对路径遗留），一个实例写入的通道 / 设备树被另一个看到。
之前只是编辑侧的问题，现在监控页也吃这个亏。已加了 404 自动降级作为兜底，
但根治得等 P3.2c 附带的"`--config-db PATH` 显式路径"选项。

### 趋势联动 ✅（2026-08-22）

**点了哪个具体点，趋势图就跟哪个**。补上 P4 挂账的最后一条 UX 缺口。

| 交付 | 说明 |
|------|------|
| `.cell` / `.kid-kv` 加 `pickable` + `data-tname="<全名>"` | 平铺、树形自有点位、子节点点值三处都能点 |
| `.split-detail` 上一次委托点击 | 一次事件挂载，`stopPropagation` 避免 kid-kv 顺带触发 kid-row 的 drillTo |
| `pickTrend(name)` | 设 `tsel.value`、追加缺失 option、给 `.picked` 视觉高亮、`drawTrend()` |
| `syncTrendOptions(names)` | 重渲染时保留用户选择、还原 `.picked` 高亮；已选点即使换页也留在 option 列表里 |
| `renderTree` 里也喂 `hist[]` | 树形模式下 renderPts 不跑，不喂就没历史线可画 |

**实测**：两实例 20s 稳定，`hist[]` 数值型点位累积正常；九单测全绿。

#### 一个走 UI 才发现的小坑

kid-kv 里的值本来就在 kid-row 的 onclick 命中范围内 —— 点值一下同时触发
"看趋势 + 下钻"两件事。事件委托必须 `stopPropagation`，否则用户点电芯电压
后 UI 会闪一下（下钻到 cell01 层）再高亮回来，看着像 bug。

#### 顺带做掉的整理

平铺模式的 grid 本来是就地拼 HTML —— 改成复用 `cellHtml(p)`，同一份 pickable
挂法两边共享，将来加动效或标签只改一处。

### P3.2c：Kafka（librdkafka 静态）+ write_point 下行 + `--config-db PATH` ⬜
### P4：实时监控树形下钻 + 层级聚合 ⬜

### 修复：重复启动静默变成两个实例 ✅（2026-08-18）

**现象**：误启第二份采集器时，Web 端口**不报错**，两个进程同时 LISTEN 同一端口，
内核在二者之间轮流分发请求。症状极具迷惑性 —— 同一个 `/api/points` 连续查询，
设备时而 5 个时而 6 个；改完配置重启，"一半生效一半没生效"（请求随机落到新旧
进程）。本次排查中就为此绕了不少弯路。

**根因**：httplib 的 `default_socket_options` 同时设置 `SO_REUSEADDR` 与
**`SO_REUSEPORT`**，后者正是"允许多个进程绑同一端口并负载均衡"的开关。

**修复**：`set_socket_options` 只设 `SO_REUSEADDR` —— 仍可在 TIME_WAIT 残留时
立即重启，但重复启动会如常报 `Address already in use`。实测第二个实例现在打印
`Web 服务启动失败：0.0.0.0:15682 无法监听（端口被占用或权限不足？）`。

### ~~待修：SIGTERM 无法停止进程~~ —— 撤销：不存在此缺陷（2026-08-18 复核）

**这一条是误判，已撤销。** 原始"证据"是这行 `ps` 输出：

```
477774 Sl  58:07  ./build/industrial_collector config/config.json
```

我把 `etime` 列的 **58:07** 读成了"收到 SIGTERM 后存活 58 分钟"，它实际是
**进程自启动以来的运行时长**。当时那个进程多半根本没收到信号 —— 用的是
`pkill -f "industrial_collector config"`，而该模式会连执行命令的 shell 一起匹配，
pkill 可能在遍历过程中把自己的父 shell 杀掉了（同一天另有多次"进程仍在"误报，
都是这个原因，见 test/TESTING.md 的停止方式说明）。

**复核方法**：精确测量"发信号 → 进程消失"的墙钟时间，三种场景各一次：

| 场景 | 信号→退出 | 优雅收尾 |
|------|----------|---------|
| 黑洞地址（TCP connect 实测需 15s 才失败） | 1419 ms | 是 |
| 正常模拟器（六协议有数据在采） | 406 ms | 是 |
| `--from-db` 编译快照驱动 | 406 ms | 是 |

三者都打印了"所有设备线程已退出，程序正常结束"，即缓存已 flush、连接已关闭。
`interruptibleSleep`（200ms 分片轮询 `running`）与信号对阻塞 syscall 的 EINTR
中断合在一起，已足以让线程及时退出。

**教训**：`ps` 的 `etime` 是运行时长不是存活时长；测"进程是否响应信号"必须
测信号发出到消失的时间差，而不是看进程还在不在。

## v1.6.0 稳健性修复（2026-07-09）✅

| 缺陷 | 修复 |
|------|------|
| **时间戳数据竞争** | `nowIso()` 用 `gmtime`、`addLog()` 用 `localtime`，二者返回**共享静态 `tm`**，却被每个设备线程并发调用。实测两线程 20 万次调用中 599 次读到对方覆写的值；错误时间戳会进 MQTT 报文与缓存 `ts` 列（时间段补传按 `ts` 检索）。全部改为 `gmtime_r` / `localtime_r`（`types.h` / `web_server.h` / `data_cache.cpp` / `main.cpp` 共 4 处） |
| **配置非原子写** | `saveConfig()` 用 `ofstream` 直接截断覆写 `config.json`，写到一半掉电即留下截断 JSON，下次启动 `loadConfig` 抛异常、程序起不来。改为 临时文件 → `fsync` → `rename` → `fsync` 目录。并发读者压测：旧写法 20497 次读取中 20204 次读到截断，新写法 1901 次读取 0 次 |
| **Web API 无鉴权** | 默认 `bind 0.0.0.0:8080`，任何人可 `POST /api/control {"action":"stop"}` 停机、`POST /api/config/full` 改写配置。新增 HTTP Basic（`web.auth_enabled/auth_user/auth_password`），`set_pre_routing_handler` 拦在所有路由之前（静态页 + 全部 `/api/*`，`/api/logs`、`/api/points` 同样泄漏数据）。定时安全比较；密码为空则 Web **拒绝启动**（失败关闭）；未开鉴权且非回环监听时启动告警；开鉴权但无 TLS 亦告警 |
| **protocol 静默回退** | `parseProtocol()` 对未知协议名 `return Protocol::MODBUS` —— `"CAN"`（大写）或 `"cann"` 笔误会静默变成一个去连 `127.0.0.1:502` 的 Modbus 设备。改为：先规范化（去空白 + 转小写，故 `"CAN"` / `" canfd "` 均可），未知值则抛异常并列出可选值、回显用户原样输入、带上设备 id。启动时 `[FATAL]` 退出；`POST /api/config/full` 返回 400 且校验在写盘之前，磁盘配置不受损 |
| **error 计数语义混淆** | `incPoll()` 原为 `good==total ? ++good_polls : ++errors` —— 一个坏寄存器就让整轮算错误，而 CAN 被动监听在总线静默时全 BAD 是合法稳态，错误数会无限增长。拆为四个量：`errors`（采集抛异常，仅 `main.cpp` 的 catch 递增）、`dead_polls`（有点但全 BAD）、`bad_points`/`total_points`（点级质量）。Web 新增"全局坏点"，设备卡片错误数悬停显示三者明细 |
| **SSE 丢唤醒** | `updatePoints()` 不持 `push_mtx_` 就 `push_seq_++` 并 `notify_all()`。SSE 线程"持锁判定 predicate 为假、尚未阻塞"的窗口内，notify 会丢失，客户端白等一个 3s 心跳。改为持锁递增、锁外通知。A/B 压测：旧写法 299/300 次丢唤醒，新写法 0/300 |
| **传输层超时被放大** | `select()` 的 `tv` 每轮循环重置 → "读 N 字节"最坏等 `N × timeout_ms`。DLT645 读帧体（`len+2`，len 可达 255）在 `timeout_ms=3000` 下最坏 **12.8 分钟**。改为一次调用共享一个绝对截止时间。顺带修两处同类缺陷：`EINTR` 不再被当成超时而 `break`（旧代码会丢掉已读字节）；改用 `poll` 替代 `select`（`fd >= FD_SETSIZE(1024)` 时 `FD_SET` 是未定义行为）。`write()` 也补了 `EINTR`/`EAGAIN` 重试（串口以 `O_NONBLOCK` 打开）。新增 `test/io_timeout_test.cpp`（`make io_timeout_test`），含旧实现对照组：字节滴流场景下旧实现 901ms / 新实现 399ms（预算 400ms） |
| **DLT 事务超时按次累加** | `io_transport` 修好后，调用方仍是乘数：`transact()` 的"跳过前导字节"循环最多 32 次（698 是 64 次）单字节读，每次都给完整 `timeout_ms`。噪声线路上 `timeout_ms=400` 实测耗时 3207ms（超预算 8×）。给两个 `transact()` 加整体 deadline，逐次把剩余毫秒传给 `read()`；预算耗尽时打印 `事务超时（Nms）：X阶段未收全`，并区分"线路噪声导致连续 32/64 字节未见 0x68"。新增 `test/dlt_deadline_test.cpp`（`make dlt_deadline_test`）|

关于 `remainingMs()` 的向上取整（`io_transport.h`）：最初用 `duration_cast` 向下取整，
导致两个问题 —— 剩余 0.9ms 时返回 0，调用方白白提前放弃；多次调用累积的截断误差让
"预算是否耗尽"变成不确定判据（`dlt_deadline_test` 曾 5 次里只过 2 次）。改为向上取整后，
等待必然覆盖到 deadline，`remain()==0` 成为确定性判据，测试 8/8 稳定通过。

补充说明：

- **鉴权默认关闭**以保持向后兼容 —— 现有部署升级后行为不变，但启动会打印安全警告。
  配套改了前端：`cfgSaveBasic()` 原本整块重建 `cfg.web`，会把 `auth_*`（以及原本就在丢的
  `debug`/`debug_root`）静默抹掉，导致"保存基础设置"= 关闭鉴权。已改为 `Object.assign` 合并。
- protocol 规范化必须发生在 `"canfd"` 简写判定**之前** —— 二者要看同一个值，否则 `"CANFD"`
  会解析成 CAN 但 `fd` 标志设不上。

### 第二轮修复（2026-07-09）✅

| 缺陷 | 修复 |
|------|------|
| **缓存懒提交** | `BATCH_FLUSH_MS` 只在下一次 `store()` 时才判定 —— 采集停止后就永远不提交，残留行 `SIGKILL` 即丢。新增后台兜底线程（每 `BATCH_FLUSH_MS/4` 醒一次），析构时 notify + join。顺带把每行 prepare/finalize 改为整批复用一个 statement（`reset` 而非 `finalize`），prune 移出事务 |
| **prune 静默丢数据** | 有界缓存必然丢老数据，但丢的若是 `id > send_cursor`（尚未补传）的行就是静默数据丢失。prune 前统计并告警，指出是保留期还是 `max_rows` 触发；被删的行都在游标之前时不告警 |
| **配置 API 回显密码** | `GET /api/config/full` 明文返回 `mqtt.password` / `web.auth_password` / `opcua.password` / `iec61850.auth_password`。改为返回哨兵 `********`；`POST` 时字段仍是哨兵则换回原值（按设备 id 对应）。用户改密码发新明文、清空发空串、没动发哨兵 |
| **`int` 截断** | `max_rows` / `max_bytes` 解析与序列化都经 `(int)` 转换，超过 2^31 的配置值会坏。改用字段自身类型（`int64_t` / `size_t`） |

> 密码哨兵的边界：设备被改了 id 时按 id 找不到旧值，此时**清空**而不是把 `********`
> 当成真密码存进去（否则会留下一个谁也不知道的密码）。已验证。

新增 `test/data_cache_test.cpp`（`make data_cache_test`）：定时兜底刷盘、statement 复用后的
写入正确性与顺序、prune 告警的正反两面。注意 prune 每 `PRUNE_EVERY(200)` 次插入才触发，
故行数不会时刻等于 `max_rows`。

---

## v1.6.0 CAN / CAN FD（2026-07-09）✅

Linux SocketCAN 被动监听，第七种协议。`protocol:"can"`，`canfd` 为 `can`+`fd:true` 的简写
（同一采集器，仅差 `CAN_RAW_FD_FRAMES` sockopt 与 64 字节负载，不复制一套 switch 分支）。

| 项目 | 说明 |
|------|------|
| **采集模型** | 不走 `ITransport`（面向字节流）。后台线程收帧按 CAN ID 缓存最新帧，`readAll()` 从缓存解码 → 复用 `runSingleLoop` 与重连逻辑 |
| **信号点表** | DBC 风格手配：`start_bit`/`bit_length`/`byte_order`/`is_signed`/`scale`/`offset`。`.dbc` 文件导入未做，可后续独立加 |
| **位序** | Intel(little) 与 Motorola(big) 双支持 + 补码符号扩展；`test/can_signal_test.cpp` 用手工推导期望值验证（`make can_signal_test`） |
| **帧新鲜度** | `stale_timeout_ms` 内未更新的信号标记 BAD；`0` = 永不过期 |
| **掉线判定** | 与轮询协议分道：被动监听只认 socket 故障（`linkOk()`），总线静默不重连。`runSingleLoop` 用 `IsPassiveCollector<>` 编译期分支 |
| **过滤器** | 按点表 CAN ID 下发 `CAN_RAW_FILTER`，超过内核上限 512 时回退全收 |
| **无新依赖** | 仅需内核头文件，`bootstrap.sh` / `third_party` 无改动 |
| **前端** | 配置页新增 CAN 协议表单 + 信号点表编辑器，与其余六协议一致 |

**已验证**：`vcan0` 上跑真实 `industrial_collector`，经典帧与 64 字节 FD 帧解码、
标准帧/扩展帧同号区分、超时失效、MQTT 报文物理值正确；静默总线 8 秒不重连（socket 只开一次），
`ip link del vcan0` 后 13ms 内判定链路故障并重连，接口恢复后自动连上。

**限制**：RAW socket 无法设置比特率，接口须先 `ip link set can0 up type can bitrate ...`；
配置里刻意不提供 `bitrate` 项。上报管道为 `double`，`bit_length > 53` 的信号会丢低位。

---

## v1.5.0 优化（2026-06-03）✅

| 项目 | 说明 |
|------|------|
| **SSE 实时推送** | `GET /api/stream` chunked EventStream；`updatePoints()` 通过 `push_cv_` 唤醒 SSE 线程；前端 `EventSource` 替换 `setInterval` |
| **SQLite 批量写入** | 内存缓冲区积攒 16 条或 2s 超时后 `BEGIN…COMMIT` 一次提交，I/O 减少 10x；析构时 `flush()` 确保数据不丢。（原为懒提交，2026-07-09 加后台兜底线程后名副其实） |
| **前端浅色主题** | CSS 变量作用域覆盖：`.content-wrapper` 用浅色变量，sidebar / header 保持深色 |
| **glibc 2.31 兼容** | `src/compat.c` 提供 `strlcpy`、`__isoc23_strtol/ul/ll/ull`，Ubuntu 20 可直接编译 |
| **Web 服务稳健** | `keep_alive_timeout=1s`、`/api/all` 合并端点、favicon 204、`fetchJ` 5s AbortController、`_refreshing` 防重入 |

---

## 大规模场景扩展（储能 / 风电）

> 背景：系统需同时支持两种场景——
> - **虚拟电厂**：多协议混合、测点少（当前架构完全够用，不动）
> - **储能 / 风电**：单柜 13000+ 测点（含 6000 电芯电压+温度），单风机 3000+ 测点，
>   同构数组结构，当前逐点配置和逐点 MQTT 均不可行。
>
> 决策：**一套系统，两种运行模式**，通过配置区分，不拆成独立系统（避免复制七协议实现）。

### 阶段一：让大规模场景可用 ✅ 已完成（2026-06-03）

#### ① Range Expansion 配置解析 ✅

在 `ModbusConfig` 中新增 `ranges[]`，运行时展开成 `points[]`，无需逐点配置。

```jsonc
"modbus": {
  "points": [...],          // 少数命名点（SOC、总电压等）照旧
  "ranges": [
    { "name_prefix": "v_",  "addr_start": 0,    "count": 6000,
      "func_code": 3, "data_type": "uint16", "scale": 0.001, "unit": "V" },
    { "name_prefix": "t_",  "addr_start": 6000, "count": 6000,
      "func_code": 3, "data_type": "int16",  "scale": 0.1,   "unit": "℃" }
  ]
}
```

**涉及文件**：`config.h`（新增 `ModbusRange` 结构体）、`config.cpp`（解析 ranges[]、展开到 points[]）

#### ② Array 发布模式（array publish mode）✅

设备级配置 `"publish_mode": "array"`，同构数组一次发一条 MQTT：

```jsonc
// topic: {prefix}/{device_id}/cell_voltage
{
  "ts": "...", "name_prefix": "v_", "start": 0,
  "scale": 0.001, "unit": "V",
  "values": [3650, 3651, 3648, ...]   // uint16 原始值，接收端乘以 scale
}
```

对比：逐点模式 6000 条 × 200B = 1.2MB/轮；array 模式 1 条 ≈ 15KB/轮，**减少 80x**。

**涉及文件**：`config.h`（`ModbusConfig` 加 `publish_mode` 字段）、`mqtt_publisher.cpp`（array 分支）

---

### 阶段二：体验完善

#### ③ Web UI 聚合视图（大规模模式专用）

- 不渲染 12000 个格子，而是**每串聚合**：min / max / avg 电压，越限电芯数
- 点击某串展开该串所有电芯（按需加载）
- 小规模虚拟电厂模式保持现有全量格子视图

#### ④ 告警阈值配置（越限点上报）

在 range 配置中加 `alarm_hi` / `alarm_lo`，采集后检测越限，单独发 MQTT 告警主题：

```jsonc
{ "name_prefix": "v_", ..., "alarm_lo": 2800, "alarm_hi": 3650 }
// 越限时发: {prefix}/{device_id}/alarm → {"point":"v_0312","value":2750,"limit":"lo"}
```

---

### 阶段三：按需扩展

#### ⑤ ClickHouse 适配器（直写时序存储，不经 MQTT）

- 目标：直接写精简版 ClickHouse（HTTP insert），跳过 MQTT broker
- 适用：超大规模场景，MQTT broker 成为瓶颈时
- 与 MQTT 并存（`output_targets: ["mqtt", "clickhouse"]`），可独立开关
- **涉及文件**：新增 `src/clickhouse_writer.cpp`，`config.h` 加 `ClickHouseConfig`

---

> 原始虚拟电厂功能规划记录于 2026-06-02。

## 1. httplib 加入 HTTPS 支持 ✅ 已完成（2026-06-02）

> OpenSSL 1.1.1w 静态编译进 `third_party/install/openssl`（bootstrap 步骤 11），
> 保持零运行时依赖/全静态。CMake 开 `CPPHTTPLIB_OPENSSL_SUPPORT=1`，链接顺序 ssl→crypto→dl。
> 配置 `web.tls_enabled/tls_cert/tls_key`；自签证书 `bash scripts/gen_cert.sh`。
> 运行时 HTTP/HTTPS 可切换，证书无效自动回退明文 HTTP（is_valid 检查）。
> 坑：paho 自带 SHA1 与 OpenSSL 冲突 → bootstrap 用 objcopy 重命名 paho 的 SHA1 符号。
> 已验证：TLS 关→HTTP / TLS 开→HTTPS / 坏证书→回退。下为原始记录：


为内置 Web 服务（cpp-httplib）增加 HTTPS/TLS。

**背景约束**：当前全项目刻意 NO-SSL —— httplib 锁 v0.12.6（v0.13+ 需 OpenSSL 3，
Ubuntu 18 仅 1.1.1），CMakeLists 里**不定义** `CPPHTTPLIB_OPENSSL_SUPPORT`（定义为 0 也会触发
`#ifdef` 引入 openssl/zlib，见注释）。加 HTTPS 需：
- 定义 `CPPHTTPLIB_OPENSSL_SUPPORT`（非 0），链接 OpenSSL 1.1.1（系统自带或静态编入 third_party）
- 提供证书/私钥配置项（self-signed 或用户提供）
- 评估是否影响"零运行时依赖、全静态链接"的目标

## 2. Debug 模式（方便调试前端）✅ 已完成（2026-06-02）

> 开启方式：命令行 `--debug` 或 `config.web.debug=true`。
> 开启后 WebServer 用 httplib `set_mount_point` 从磁盘 `web/` 目录实时伺服，改完刷新即可，
> 无需重新编译；release 仍用编译内嵌字节数组。默认根目录由 CMake 注入 `IC_WEB_SOURCE_DIR`
> （源码 web/ 绝对路径），可用 `web.debug_root` 覆盖。已 docker 验证两模式行为。下为原始记录：


加一个调试开关，便于改前端时免重新编译。

**背景**：当前 `web/{index.html,assets/*}` 在编译时由 `scripts/gen_web.py` 打包进
`web_assets_gen.h`（字节数组），改前端要重新 `make`。
**思路**：debug 模式下 WebServer 直接从磁盘 `web/` 目录读取并伺服（`svr_->set_mount_point`
或运行时读文件），改完刷新即可；release 仍用编译内嵌。开关可走配置项或命令行 `--debug`。

## 3. 配置页面 + AdminLTE 风格 UI ✅ 已完成（2026-06-02）

> 前端完整重构为 AdminLTE 风格（深色主题）三栏 SPA：实时监控 / 配置管理 / 系统日志。
> 配置管理页支持 MQTT / Web / 缓存 / 日志四项基础设置 CRUD，以及设备完整 CRUD（含全部六种协议
> 的参数表单和采集点管理；DLT645/698 电表用 accordion 两级结构）。
> 后端新增 `GET /api/config/full`（返回完整 AppConfig JSON）和 `POST /api/config/full`
> （接收完整 JSON，校验后保存至 config.json 并更新 SharedState）。
> 存储仍为 JSON（"考虑 SQLite"的描述是可选项，现有 data_cache.db 已使用 SQLite；
> 配置 CRUD 完整落地，SQLite 迁移可后续独立进行）。
> 已编译验证：无警告无错误。下为原始需求记录：

Web UI 增加配置编辑页面（增删设备/点表等），配置存储考虑从 JSON 迁移到 SQLite。

**背景**：当前配置为 `config.json`（`devices[]` 结构，`config.cpp` 解析/保存，
Web 仅支持热更新 topic/qos/poll_interval 几项）。
**思路**：
- 前端配置页：完整 CRUD（设备、协议参数、点表）
- 后端：SQLite 存配置，替代/兼容 JSON；需引入 sqlite3（third_party 静态库）
- 注意与现有 `loadConfig/saveConfig`、热更新机制的衔接

## 4. 本地缓存 1 天数据 + 断点续传 ✅ 已完成（2026-06-02）

> 实现见 `src/data_cache.cpp`（SQLite 时序缓存）+ `src/mqtt_publisher.cpp`（缓存写入/订阅/回放线程）。
> 配置 `cache` 段；指令主题 `{prefix}/cmd/replay`，历史主题 `{prefix}/history`。
> 三种续传模式（普通续传从持久化断点 / `since` 游标 / `start,end` 时间段）均经 docker 验证，
> 含断 broker 离线缓存 + 重连重订阅 + 中断保存断点。下方为原始需求记录：


本地缓存最近 1 天的采集数据；该缓存数据走**另一个 MQTT 主题**，收到指令后陆续补传
（实现断点续传 / store-and-forward）。

**背景**：当前 `MqttPublisher` 实时发布到 `{prefix}/{device}/{point}` 与 `/batch`，
断线期间数据丢失（仅靠 paho 自动重连，无补传）。
**思路**：
- 本地持久化最近 1 天数据（SQLite 时序表 / 环形文件），按时间索引
- 正常实时主题不变；新增"历史补传"主题（如 `{prefix}/history`）
- 订阅一个指令主题，收到请求（含时间范围/断点位置）后，从缓存按序、限速补发
- 关注：磁盘占用上限、补传不阻塞实时采集（独立线程）、QoS/确认机制
