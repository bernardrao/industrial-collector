FROM python:3.12-slim

# 各协议模拟器依赖：
#   pymodbus  — Modbus TCP slave
#   asyncua   — OPC UA server
#   c104      — IEC 60870-5-104 server（lib60870 的 Python 封装）
# DLT645/DLT698 为自写的纯 socket 模拟器，无额外依赖。
# 钉版本：pymodbus 3.13 移除了同步 StartTcpServer / ModbusSlaveContext，
# 3.6.9 为稳定 API（与 modbus_sim.py 匹配）。asyncua/c104 钉已验证版本。
RUN pip install --no-cache-dir \
        "pymodbus==3.6.9" \
        "asyncua==1.1.8" \
        "c104==2.2.1"

WORKDIR /sim
COPY simulators/ /sim/

# run_all.py 并发启动全部 TCP 模拟器
CMD ["python", "-u", "run_all.py"]
