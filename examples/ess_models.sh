#!/bin/bash
# examples/ess_models.sh — 储能电站设备规格完整示例
#
# 建立一套七层设备规格并实例化成设备树，用来演示"模型驱动"的完整链路：
#
#   CELL001 电芯    ← 叶子：属性(厂商/容量…) + 数据点(电压/温度)
#   PACK001 电池包  ← 24 × CELL001
#   CLST001 电池簇  ← 4 × PACK001
#   CTL001  控制器  ← 叶子：多属性 + 多数据点
#   RACK001 电池舱  ← 8 × CLST001
#   RACK002 电池舱  ← 7 × CLST001 + 1 × CTL001   （异构组合）
#   BOOST01 升压站  ← 叶子：多属性 + 多数据点
#
#   电站 = 1 升压站 + 4 台 RACK001 + 4 台 RACK002
#        = 6074 个节点，其中 5760 个电芯
#
# 用法:
#   bash examples/ess_models.sh [基地址]
#   bash examples/ess_models.sh http://127.0.0.1:15688
#
# 若开了 Web 鉴权，设置环境变量：
#   AUTH='-u admin:密码' bash examples/ess_models.sh
set -e

B="${1:-http://127.0.0.1:15688}"
AUTH="${AUTH:-}"
J='Content-Type: application/json'

G='\033[0;32m'; Y='\033[1;33m'; R='\033[0;31m'; N='\033[0m'
info() { echo -e "${G}[✓]${N} $*"; }
step() { echo -e "\n${G}══${N} $* ${G}══${N}"; }
warn() { echo -e "${Y}[!]${N} $*"; }

# 统一发请求并检查 ok 字段：后端把失败原因（成环、外键、超限）写得很清楚，
# 出错时原样打出来，别用"操作失败"盖掉
post() {
    local url="$1" data="$2"
    local r
    r=$(curl -s $AUTH -X POST "$B$url" -H "$J" -d "$data")
    if echo "$r" | grep -q '"ok":false'; then
        echo -e "${R}[✗]${N} POST $url"
        echo "$r" | python3 -c "import sys,json;print('    ',json.load(sys.stdin).get('error','?'))" 2>/dev/null || echo "    $r"
        exit 1
    fi
    echo "$r"
}

echo "目标: $B"
curl -s $AUTH -o /dev/null -w "" "$B/api/models" 2>/dev/null || {
    echo -e "${R}[✗]${N} 连不上 $B —— 服务没起来？端口对吗？"; exit 1; }

# ── 1. 叶子规格：电芯 ────────────────────────────────────────────────────────
step "1/4  建立设备规格"

post /api/models '{
  "code":"CELL001","name":"磷酸铁锂电芯","kind":"cell",
  "attrs":[
    {"key":"vendor","value":"CATL"},
    {"key":"chemistry","value":"LFP"},
    {"key":"nominal_capacity_ah","value":"280"},
    {"key":"nominal_voltage_v","value":"3.2"}
  ],
  "points":[
    {"key":"voltage","name":"单体电压","unit":"V"},
    {"key":"temp","name":"单体温度","unit":"℃"}
  ]}' > /dev/null
info "CELL001 电芯（4 属性 / 2 数据点）"

# ── 2. 组合模型 ──────────────────────────────────────────────────────────────
# 组合关系结构化存储；PACK001.CELL001024 那种把数量编进名字的写法只作显示，
# 靠解析字符串还原结构在型号含数字或数量超 999 时会出歧义。
post /api/models '{
  "code":"PACK001","name":"电池包","kind":"pack",
  "attrs":[{"key":"cell_count","value":"24"},{"key":"topology","value":"24S1P"}],
  "points":[
    {"key":"pack_voltage","name":"电池包电压","unit":"V"},
    {"key":"pack_current","name":"电池包电流","unit":"A"},
    {"key":"cell_v_max","name":"最高单体电压","unit":"V"},
    {"key":"cell_v_min","name":"最低单体电压","unit":"V"},
    {"key":"cell_t_max","name":"最高单体温度","unit":"℃"}
  ]}' > /dev/null
post /api/models/PACK001/children '{"model":"CELL001","count":24}' > /dev/null
info "PACK001 电池包 ← 24×CELL001"

post /api/models '{
  "code":"CLST001","name":"电池簇","kind":"cluster",
  "attrs":[{"key":"pack_count","value":"4"}],
  "points":[
    {"key":"cluster_voltage","name":"簇电压","unit":"V"},
    {"key":"cluster_current","name":"簇电流","unit":"A"},
    {"key":"soc","name":"荷电状态","unit":"%"},
    {"key":"soh","name":"健康度","unit":"%"},
    {"key":"insulation_res","name":"绝缘电阻","unit":"kΩ"}
  ]}' > /dev/null
post /api/models/CLST001/children '{"model":"PACK001","count":4}' > /dev/null
info "CLST001 电池簇 ← 4×PACK001"

# ── 3. 控制器（叶子，多属性多数据点）──
post /api/models '{
  "code":"CTL001","name":"舱级控制器","kind":"ctl",
  "attrs":[
    {"key":"vendor","value":"Sungrow"},
    {"key":"model_no","value":"SC2000UD"},
    {"key":"protocol","value":"modbus-tcp"},
    {"key":"fw_version","value":"2.4.1"}
  ],
  "points":[
    {"key":"run_state","name":"运行状态","unit":""},
    {"key":"fault_code","name":"故障码","unit":""},
    {"key":"active_power","name":"有功功率","unit":"kW"},
    {"key":"reactive_power","name":"无功功率","unit":"kVar"},
    {"key":"ac_voltage","name":"交流电压","unit":"V"},
    {"key":"ac_current","name":"交流电流","unit":"A"},
    {"key":"cabinet_temp","name":"舱内温度","unit":"℃"},
    {"key":"humidity","name":"舱内湿度","unit":"%"}
  ]}' > /dev/null
info "CTL001 舱级控制器（4 属性 / 8 数据点）"

# ── 4. 两种电池舱：同构与异构 ──
post /api/models '{
  "code":"RACK001","name":"标准电池舱","kind":"rack",
  "attrs":[{"key":"cluster_count","value":"8"},{"key":"cooling","value":"liquid"}],
  "points":[
    {"key":"rack_voltage","name":"舱直流母线电压","unit":"V"},
    {"key":"rack_power","name":"舱功率","unit":"kW"},
    {"key":"rack_soc","name":"舱 SOC","unit":"%"}
  ]}' > /dev/null
post /api/models/RACK001/children '{"model":"CLST001","count":8}' > /dev/null
info "RACK001 标准电池舱 ← 8×CLST001"

# RACK002 是异构组合：7 簇 + 1 控制器。
# 这正是 ON CONFLICT(model,ord) 会踩坑的形状 —— 两次追加若 ord 相同，
# 后一次会静默替换前一次；后端的 appendModelChild 自动递增 ord 避免了这一点。
post /api/models '{
  "code":"RACK002","name":"带控制器电池舱","kind":"rack",
  "attrs":[{"key":"cluster_count","value":"7"},{"key":"has_controller","value":"true"}],
  "points":[
    {"key":"rack_voltage","name":"舱直流母线电压","unit":"V"},
    {"key":"rack_power","name":"舱功率","unit":"kW"},
    {"key":"rack_soc","name":"舱 SOC","unit":"%"}
  ]}' > /dev/null
post /api/models/RACK002/children '{"model":"CLST001","count":7}' > /dev/null
post /api/models/RACK002/children '{"model":"CTL001","count":1}'  > /dev/null
info "RACK002 带控制器电池舱 ← 7×CLST001 + 1×CTL001（异构）"

post /api/models '{
  "code":"BOOST01","name":"升压站","kind":"boost",
  "attrs":[
    {"key":"transformer_capacity_kva","value":"2500"},
    {"key":"voltage_ratio","value":"0.4/35kV"},
    {"key":"vendor","value":"TBEA"}
  ],
  "points":[
    {"key":"hv_voltage","name":"高压侧电压","unit":"kV"},
    {"key":"hv_current","name":"高压侧电流","unit":"A"},
    {"key":"lv_voltage","name":"低压侧电压","unit":"V"},
    {"key":"lv_current","name":"低压侧电流","unit":"A"},
    {"key":"active_power","name":"有功功率","unit":"kW"},
    {"key":"power_factor","name":"功率因数","unit":""},
    {"key":"oil_temp","name":"油温","unit":"℃"},
    {"key":"breaker_state","name":"断路器状态","unit":""}
  ]}' > /dev/null
info "BOOST01 升压站（3 属性 / 8 数据点）"

# ── 环检测演示：让 CELL001 反过来包含 RACK001，应被拒绝 ──
step "2/5  组合环检测（应当拒绝）"
r=$(curl -s $AUTH -X POST "$B/api/models/CELL001/children" -H "$J" \
        -d '{"model":"RACK001","count":1}')
if echo "$r" | grep -q '"ok":false'; then
    info "已拒绝：$(echo "$r" | python3 -c 'import sys,json;print(json.load(sys.stdin)["error"])')"
else
    warn "环检测没生效，这是个 BUG"
fi

# ── 采集任务 + 地址绑定 ──────────────────────────────────────────────────────
# 光有规格和树是【编译不出东西】的：编译器要知道每个测点归哪个任务、地址怎么算。
# 早先这一段缺失，脚本跑完看着一切正常，一点「生效」才发现"尚无编译版本"。
step "3/5  采集任务与地址绑定"

post /api/tasks '{
  "id":"bms_modbus","protocol":"modbus","interval_ms":2000,"reconnect_sec":5,
  "endpoint":{"host":"127.0.0.1","port":15650,"unit_id":1,"timeout":3,
              "publish_mode":"batch","ranges":[]}
}' > /dev/null
info "采集任务 bms_modbus → 127.0.0.1:15650（模拟器见 test/docker/simulators/ess_modbus_sim.py）"

# 地址映射表 —— 与 ess_modbus_sim.py 的基址常量【必须逐条对上】。
# 改一边就要改另一边；对不上的症状是"连接正常但点全 BAD"。
#
#   CELL  电压 10000 + gidx*2      温度 40000 + gidx
#   PACK  46000 + gidx*8 + k       CLST 48000 + gidx*8 + k
#   RACK001 49000 + gidx*8 + k     RACK002 49100 + gidx*8 + k
#   CTL   49200 + gidx*16 + k      BOOST 49300 + k
#
# 用 gidx_<MODEL>（全局实例序号）而不是 idx_<MODEL>（同父下的序号）：地址在整
# 个电站里必须唯一，而 idx 每换一个父节点就从 0 重来。
# RACK001 与 RACK002 是两个独立的 gidx 计数器（编译器按模型 code 分别累加），
# 所以各占一段基址，不能共用。
bind() {   # bind 模型 点键 公式 类型 scale
    post "/api/models/$1/bindings" \
        "{\"point_key\":\"$2\",\"task\":\"bms_modbus\",\"addr_formula\":\"$3\",\"dtype\":\"$4\",\"scale\":$5,\"offset\":0}" > /dev/null
}

bind CELL001 voltage "10000 + gidx_CELL001*2" uint16 0.001
bind CELL001 temp    "40000 + gidx_CELL001"   int16  0.1
info "CELL001  2 点（5760 个实例 → 电压 10000..21518，温度 40000..45759）"

bind PACK001 pack_voltage "46000 + gidx_PACK001*8"     uint16 0.01
bind PACK001 pack_current "46000 + gidx_PACK001*8 + 1" int16  0.1
bind PACK001 cell_v_max   "46000 + gidx_PACK001*8 + 2" uint16 0.001
bind PACK001 cell_v_min   "46000 + gidx_PACK001*8 + 3" uint16 0.001
bind PACK001 cell_t_max   "46000 + gidx_PACK001*8 + 4" int16  0.1
info "PACK001  5 点（240 个实例）"

bind CLST001 cluster_voltage "48000 + gidx_CLST001*8"     uint16 0.1
bind CLST001 cluster_current "48000 + gidx_CLST001*8 + 1" int16  0.1
bind CLST001 soc             "48000 + gidx_CLST001*8 + 2" uint16 0.1
bind CLST001 soh             "48000 + gidx_CLST001*8 + 3" uint16 0.1
bind CLST001 insulation_res  "48000 + gidx_CLST001*8 + 4" uint16 1
info "CLST001  5 点（60 个实例）"

bind RACK001 rack_voltage "49000 + gidx_RACK001*8"     uint16 0.1
bind RACK001 rack_power   "49000 + gidx_RACK001*8 + 1" int16  1
bind RACK001 rack_soc     "49000 + gidx_RACK001*8 + 2" uint16 0.1
bind RACK002 rack_voltage "49100 + gidx_RACK002*8"     uint16 0.1
bind RACK002 rack_power   "49100 + gidx_RACK002*8 + 1" int16  1
bind RACK002 rack_soc     "49100 + gidx_RACK002*8 + 2" uint16 0.1
info "RACK001 / RACK002  各 3 点（各 4 个实例，两段独立基址）"

bind CTL001 run_state      "49200 + gidx_CTL001*16"     uint16 1
bind CTL001 fault_code     "49200 + gidx_CTL001*16 + 1" uint16 1
bind CTL001 active_power   "49200 + gidx_CTL001*16 + 2" int16  1
bind CTL001 reactive_power "49200 + gidx_CTL001*16 + 3" int16  1
bind CTL001 ac_voltage     "49200 + gidx_CTL001*16 + 4" uint16 0.1
bind CTL001 ac_current     "49200 + gidx_CTL001*16 + 5" uint16 0.1
bind CTL001 cabinet_temp   "49200 + gidx_CTL001*16 + 6" int16  0.1
bind CTL001 humidity       "49200 + gidx_CTL001*16 + 7" uint16 0.1
info "CTL001   8 点（4 个实例）"

bind BOOST01 hv_voltage    "49300"     uint16 0.1
bind BOOST01 hv_current    "49300 + 1" uint16 0.1
bind BOOST01 lv_voltage    "49300 + 2" uint16 0.1
bind BOOST01 lv_current    "49300 + 3" uint16 0.1
bind BOOST01 active_power  "49300 + 4" int16  1
bind BOOST01 power_factor  "49300 + 5" uint16 0.001
bind BOOST01 oil_temp      "49300 + 6" int16  0.1
bind BOOST01 breaker_state "49300 + 7" uint16 1
info "BOOST01  8 点（1 个实例）"

# ── 实例化 ───────────────────────────────────────────────────────────────────
step "4/5  实例化设备树"

# 先问规模，心里有数再建
for m in RACK001 RACK002 BOOST01; do
    n=$(curl -s $AUTH "$B/api/models/$m/instance_size" \
        | python3 -c 'import sys,json;print(json.load(sys.stdin)["nodes"])')
    echo "    $m 展开后 $n 个节点"
done

# 已经建过就跳过 —— 脚本要能重复跑：改了绑定想重新「生效」时，不该顺带再长
# 出一个电站来。判据是根节点 station 在不在，不是"建失败了就算了"。
ROOT=$(curl -s $AUTH "$B/api/tree" | python3 -c '
import sys,json
n=[x for x in json.load(sys.stdin) if x["path"]=="station"]
print(n[0]["id"] if n else "")')
if [ -n "$ROOT" ]; then
    warn "设备树已存在（station id=$ROOT），跳过实例化"
    SKIP_TREE=1
else
    ROOT=$(post /api/tree/node '{"parent_id":null,"code":"station","name":"示范储能电站"}' \
           | python3 -c 'import sys,json;print(json.load(sys.stdin)["id"])')
    info "根节点 station（id=$ROOT）"
fi

if [ -z "${SKIP_TREE:-}" ]; then
post /api/tree/instantiate "{\"parent_id\":$ROOT,\"code\":\"boost01\",\"name\":\"升压站\",\"model\":\"BOOST01\"}" >/dev/null
info "升压站 ×1"

for i in 1 2 3 4; do
    post /api/tree/instantiate \
        "{\"parent_id\":$ROOT,\"code\":\"rack$(printf %02d $i)\",\"name\":\"${i}号标准舱\",\"model\":\"RACK001\"}" >/dev/null
done
info "RACK001 标准舱 ×4"

for i in 5 6 7 8; do
    post /api/tree/instantiate \
        "{\"parent_id\":$ROOT,\"code\":\"rack$(printf %02d $i)\",\"name\":\"${i}号控制舱\",\"model\":\"RACK002\"}" >/dev/null
done
info "RACK002 控制舱 ×4"
fi

# ── 汇总 ─────────────────────────────────────────────────────────────────────
step "5/5  结果"
curl -s $AUTH "$B/api/tree?flat=1" | python3 -c '
import sys,json
n=json.load(sys.stdin)
by={}
for x in n: by.setdefault(x["model"] or "(自由节点)",0); by[x["model"] or "(自由节点)"]+=1
print(f"  设备树共 {len(n)} 个节点：")
for k,v in sorted(by.items(),key=lambda kv:-kv[1]):
    print(f"    {k:<16} {v:>6}")
print()
deep=[x["path"] for x in n if x["path"].count(".")>=4][:3]
print("  最深路径示例：")
for p in deep: print("   ",p)
'
echo
echo "打开 Web 界面 → 设备组装 → 设备规格管理 / 设备树 查看"
