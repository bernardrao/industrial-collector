"""
储能电站 Modbus TCP slave 模拟器（pymodbus）
端口 1503（宿主机 15650），unit/slave id = 1

给 examples/ess_models.sh 建出来的那棵设备树当数据源：
    电站 = 1 升压站 + 4×RACK001 + 4×RACK002
         = 60 簇 / 240 包 / 5760 电芯

── 为什么值不是随机数 ──────────────────────────────────────────────────────
寄存器值按【设备树拓扑】算出来，层层自洽：

    包电压   = 它那 24 个电芯电压之和
    包内极值 = 那 24 个电芯的真实 max / min
    簇电压   = 它那 4 个包的电压之和
    舱功率   = 舱下各簇功率之和

拓扑从全局序号反推即可，不必知道树本身：

    pack_gidx    = cell_gidx // 24
    cluster_gidx = pack_gidx // 4

这样 P4 的"层级聚合"就有了**可核对的标准答案** —— 界面上包电压若不等于 24 个
电芯之和，那是聚合算错了，而不是"模拟器给的数不好看"。填随机数的话，聚合写错
了也看不出来。

── 与 modbus_sim.py 的分工 ─────────────────────────────────────────────────
两个都是 pymodbus（独立于采集器的 libmodbus），故都算**真互操作**。区别在用途：
  modbus_sim.py      3 个点，验协议正确性
  ess_modbus_sim.py  11541 个点，验规模、分组合并与层级聚合
"""
import math
import struct
import time

from pymodbus.datastore import (ModbusSequentialDataBlock,
                                ModbusSlaveContext, ModbusServerContext)
from pymodbus.server import StartTcpServer

# ── 规模（与 examples/ess_models.sh 一致）──────────────────────────────────
CELLS_PER_PACK    = 24
PACKS_PER_CLUSTER = 4
N_RACK001, N_RACK002 = 4, 4
CLST_PER_RACK001, CLST_PER_RACK002 = 8, 7      # RACK002 是 7 簇 + 1 控制器
N_CLUSTER = N_RACK001 * CLST_PER_RACK001 + N_RACK002 * CLST_PER_RACK002   # 60
N_PACK    = N_CLUSTER * PACKS_PER_CLUSTER                                 # 240
N_CELL    = N_PACK * CELLS_PER_PACK                                       # 5760
N_CTL     = N_RACK002                                                     # 4

# ── 地址基址（必须与 ess_models.sh 里的绑定公式逐条对上）──────────────────
# 改这里就要同步改那边，反之亦然。两处对不上的症状是"点全 BAD 但连接正常"。
A_CELL_V   = 10000   # + gidx*2      uint16 scale 0.001
A_CELL_T   = 40000   # + gidx        int16  scale 0.1
A_PACK     = 46000   # + gidx*8 + k
A_CLST     = 48000   # + gidx*8 + k
A_RACK1    = 49000   # + gidx*8 + k
A_RACK2    = 49100   # + gidx*8 + k
A_CTL      = 49200   # + gidx*16 + k
A_BOOST    = 49300   # + k
REG_TOP    = 49400   # 数据块大小（留了余量）

# 电芯温度基址 40000 与包基址 46000 之间只有 40000+5760=45760 → 46000 共 240 空位。
# 电芯数若上调，先把 A_PACK 一起抬高，否则温度会悄悄盖到包数据上。


def u16(v):
    """夹到 uint16。模拟器不该因为一个越界值把整个服务打挂。"""
    return max(0, min(0xFFFF, int(round(v))))


def i16(v):
    v = max(-32768, min(32767, int(round(v))))
    return v & 0xFFFF          # 负数按补码放进寄存器，采集器按 int16 解回


def f32_words(value):
    """float32 → [高字, 低字]，与采集器 decodeRegisters 的大端字序一致"""
    raw = struct.unpack(">I", struct.pack(">f", value))[0]
    return [(raw >> 16) & 0xFFFF, raw & 0xFFFF]


def build_registers(t):
    """按时刻 t（秒）算出整片寄存器。

    t 只用来做缓慢起伏，让界面上的曲线会动；拓扑一致性在任何 t 都成立。
    """
    hr = [0] * REG_TOP

    # ── 电芯：先全部算出来，上层才好求和 ──────────────────────────────
    # 电压 3.15~3.35V 之间按序号+时间铺开；同一个包内的 24 个电芯略有差异，
    # 这样 cell_v_max / cell_v_min 才不是同一个数。
    cell_v = [0.0] * N_CELL       # V
    cell_t = [0.0] * N_CELL       # ℃
    for g in range(N_CELL):
        phase = (g % CELLS_PER_PACK) / CELLS_PER_PACK
        v = 3.25 + 0.05 * math.sin(t / 30.0 + g * 0.37) + 0.02 * (phase - 0.5)
        cell_v[g] = v
        cell_t[g] = 25.0 + 6.0 * math.sin(t / 90.0 + g * 0.011) + 0.5 * phase

        hr[A_CELL_V + g * 2] = u16(v * 1000)       # scale 0.001
        hr[A_CELL_T + g]     = i16(cell_t[g] * 10) # scale 0.1

    # ── 电池包：5 点，值来自它那 24 个电芯 ────────────────────────────
    pack_v = [0.0] * N_PACK
    for p in range(N_PACK):
        s = p * CELLS_PER_PACK
        vs = cell_v[s:s + CELLS_PER_PACK]
        ts = cell_t[s:s + CELLS_PER_PACK]
        pv = sum(vs)                                    # ≈ 78V
        pack_v[p] = pv
        cur = 40.0 * math.sin(t / 120.0 + p * 0.05)     # 充放电，可正可负
        b = A_PACK + p * 8
        hr[b + 0] = u16(pv * 100)          # pack_voltage  scale 0.01
        hr[b + 1] = i16(cur * 10)          # pack_current  scale 0.1（有正负）
        hr[b + 2] = u16(max(vs) * 1000)    # cell_v_max    scale 0.001
        hr[b + 3] = u16(min(vs) * 1000)    # cell_v_min    scale 0.001
        hr[b + 4] = i16(max(ts) * 10)      # cell_t_max    scale 0.1

    # ── 电池簇：5 点，电压 = 它那 4 个包之和 ──────────────────────────
    clst_v = [0.0] * N_CLUSTER
    for c in range(N_CLUSTER):
        s = c * PACKS_PER_CLUSTER
        cv = sum(pack_v[s:s + PACKS_PER_CLUSTER])       # ≈ 312V
        clst_v[c] = cv
        cur = 35.0 * math.sin(t / 120.0 + c * 0.2)
        b = A_CLST + c * 8
        hr[b + 0] = u16(cv * 10)                        # cluster_voltage scale 0.1
        hr[b + 1] = i16(cur * 10)                       # cluster_current scale 0.1
        hr[b + 2] = u16((55 + 20 * math.sin(t / 200.0 + c)) * 10)  # soc scale 0.1
        hr[b + 3] = u16((97.5 - 0.01 * c) * 10)                    # soh scale 0.1
        hr[b + 4] = u16(1200 + 30 * math.sin(t / 300.0 + c))       # 绝缘电阻 kΩ

    # ── 电池舱：3 点，按舱下各簇聚合 ──────────────────────────────────
    # RACK001 与 RACK002 的 gidx 是两个独立计数器（compileTree 按【模型 code】
    # 分别累加），所以两者各占一段基址，不能共用。
    ci = 0
    for kind, base, n_rack, n_clst in (("R1", A_RACK1, N_RACK001, CLST_PER_RACK001),
                                       ("R2", A_RACK2, N_RACK002, CLST_PER_RACK002)):
        for r in range(n_rack):
            mine = list(range(ci, ci + n_clst))
            ci += n_clst
            rv = sum(clst_v[i] for i in mine) / len(mine)     # 并联 → 取均值
            pw = sum(clst_v[i] * 35.0 * math.sin(t / 120.0 + i * 0.2)
                     for i in mine) / 1000.0                   # kW，可正可负
            b = base + r * 8
            hr[b + 0] = u16(rv * 10)                           # rack_voltage scale 0.1
            hr[b + 1] = i16(pw)                                # rack_power   scale 1
            hr[b + 2] = u16((55 + 20 * math.sin(t / 200.0 + r)) * 10)  # soc scale 0.1

    # ── 舱级控制器：8 点 ──────────────────────────────────────────────
    for k in range(N_CTL):
        b = A_CTL + k * 16
        hr[b + 0] = 2                                   # run_state 2=运行
        hr[b + 1] = 0                                   # fault_code
        hr[b + 2] = i16(300 * math.sin(t / 120.0 + k))  # active_power kW
        hr[b + 3] = i16(60 * math.sin(t / 150.0 + k))   # reactive_power kVar
        hr[b + 4] = u16(4000 + 20 * math.sin(t / 60.0)) # ac_voltage scale 0.1 → 400V
        hr[b + 5] = u16(abs(4500 * math.sin(t / 120.0 + k)))  # ac_current scale 0.1
        hr[b + 6] = i16((32 + 4 * math.sin(t / 100.0 + k)) * 10)  # cabinet_temp
        hr[b + 7] = u16((45 + 8 * math.sin(t / 400.0 + k)) * 10)  # humidity

    # ── 升压站：8 点 ──────────────────────────────────────────────────
    b = A_BOOST
    hr[b + 0] = u16(350 + 3 * math.sin(t / 200.0))      # hv_voltage scale 0.1 → 35.0kV
    hr[b + 1] = u16(abs(300 * math.sin(t / 120.0)))     # hv_current scale 0.1
    hr[b + 2] = u16(4000 + 20 * math.sin(t / 60.0))     # lv_voltage scale 0.1 → 400V
    hr[b + 3] = u16(abs(26000 * math.sin(t / 120.0)))   # lv_current scale 0.1
    hr[b + 4] = i16(1200 * math.sin(t / 120.0))         # active_power kW
    hr[b + 5] = u16(985 + 10 * math.sin(t / 300.0))     # power_factor scale 0.001
    hr[b + 6] = i16((55 + 6 * math.sin(t / 400.0)) * 10)  # oil_temp scale 0.1
    hr[b + 7] = 1                                        # breaker_state 1=合闸
    return hr


class LiveBlock(ModbusSequentialDataBlock):
    """按需重算的数据块。

    5760 个电芯每秒重算一遍要几十毫秒，而采集器一轮会打上百个读请求 —— 每个
    请求都重算既浪费，又会让【同一轮采集里先后读到的寄存器来自不同时刻】，
    包电压就对不上它那 24 个电芯之和了，层级聚合的标准答案随之失效。
    故按 REFRESH_SEC 节流：一轮采集内看到的是同一份快照。
    """
    REFRESH_SEC = 1.0

    def __init__(self):
        super().__init__(0, build_registers(0.0))
        self._t0 = time.time()
        self._last = 0.0

    def getValues(self, address, count=1):
        now = time.time()
        if now - self._last >= self.REFRESH_SEC:
            self.values = build_registers(now - self._t0)
            self._last = now
        return super().getValues(address, count)


def run(host="0.0.0.0", port=1503):
    live = LiveBlock()
    store = ModbusSlaveContext(
        hr=live,
        ir=live,                       # 输入寄存器同值，绑定用 fc3/fc4 都行
        co=ModbusSequentialDataBlock(0, [0] * 16),
        di=ModbusSequentialDataBlock(0, [0] * 16),
        zero_mode=True,                # 地址 N 直接映射 values[N]，无 +1 偏移
    )
    ctx = ModbusServerContext(slaves={1: store}, single=False)
    print(f"[ess_modbus] 储能站 TCP slave 启动 {host}:{port} unit=1  "
          f"电芯 {N_CELL} / 包 {N_PACK} / 簇 {N_CLUSTER}", flush=True)
    StartTcpServer(context=ctx, address=(host, port))


if __name__ == "__main__":
    run()
