"""
Modbus TCP slave 模拟器（pymodbus）
端口 1502，unit/slave id = 1

寄存器布局（与 config.docker.json 的 modbus 点表对应）：
  holding 0-1 : float32  炉温     = 25.5  (big-endian word order, scale 0.1 → raw 255)
  holding 2-3 : float32  压力     = 1.23  (scale 0.01 → 但这里直接放真实浮点)
  holding 4   : uint16   泵转速   = 1450
说明：采集器按 float32（高字在前）解析 holding 0-1/2-3。
"""
import struct
from pymodbus.datastore import (ModbusSequentialDataBlock,
                                ModbusSlaveContext, ModbusServerContext)
from pymodbus.server import StartTcpServer


def f32_words(value):
    """float32 → [hi_word, lo_word]（大端字序，与采集器 decodeRegisters 一致）"""
    raw = struct.unpack(">I", struct.pack(">f", value))[0]
    return [(raw >> 16) & 0xFFFF, raw & 0xFFFF]


def build_context():
    # holding registers
    hr = [0] * 16
    hr[0:2] = f32_words(255.0)   # 炉温 raw（采集器 scale 0.1 → 25.5 ℃）
    hr[2:4] = f32_words(123.0)   # 压力 raw（采集器 scale 0.01 → 1.23 MPa）
    hr[4]   = 1450               # 泵转速 rpm

    # zero_mode=True：地址 N 直接映射 values[N]，避免 pymodbus 默认的 +1 偏移
    store = ModbusSlaveContext(
        hr=ModbusSequentialDataBlock(0, hr),
        ir=ModbusSequentialDataBlock(0, hr),   # input registers 同值
        co=ModbusSequentialDataBlock(0, [0]*16),
        di=ModbusSequentialDataBlock(0, [1, 0, 1, 0]*4),
        zero_mode=True,
    )
    return ModbusServerContext(slaves={1: store}, single=False)


def run(host="0.0.0.0", port=1502):
    print(f"[modbus] TCP slave 启动 {host}:{port} unit=1", flush=True)
    StartTcpServer(context=build_context(), address=(host, port))


if __name__ == "__main__":
    run()
