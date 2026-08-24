"""
IEC 60870-5-104 server 模拟器（c104，lib60870 封装）
端口 2404，common address (CA) = 1

测点（与 config.docker.json 的 iec104 点表对应）：
  IOA 1001  M_ME_NC_1 (type 13, 短浮点)  电网电压 = 220.5
  IOA 1002  M_ME_NC_1 (type 13, 短浮点)  电网电流 = 15.2

注意：c104 默认走平衡/非平衡传输，采集器需先发 STARTDT。
若采集器握手不完整，这里会暴露出来——属真实发现，不是模拟器坏。
"""
import time

try:
    import c104
except ImportError as e:
    raise SystemExit("缺少 c104，请确认 Dockerfile 已 pip install c104") from e


def run(port=2404):
    server = c104.Server(ip="0.0.0.0", port=port)

    station = server.add_station(common_address=1)

    volt = station.add_point(io_address=1001, type=c104.Type.M_ME_NC_1)
    curr = station.add_point(io_address=1002, type=c104.Type.M_ME_NC_1)
    volt.value = 220.5
    curr.value = 15.2

    server.start()
    print(f"[iec104] server 启动 :{port} CA=1 (IOA 1001/1002)", flush=True)

    t = 0.0
    while True:
        time.sleep(2)
        t += 0.1
        volt.value = 220.5 + (t % 4)
        curr.value = 15.2 + (t % 2) * 0.1
        # 显式上送两个点（不依赖隐式 report，确保 1001/1002 都被传输）
        volt.transmit(cause=c104.Cot.SPONTANEOUS)
        curr.transmit(cause=c104.Cot.SPONTANEOUS)


if __name__ == "__main__":
    run()
