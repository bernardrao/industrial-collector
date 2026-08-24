"""
DL/T 645-2007 + DL/T 698.45 TCP 网关模拟器（自写纯 socket）

⚠️ 一致性测试专用，非规约符合性验证。
   本模拟器与 src/dlt645_collector.cpp / dlt698_collector.cpp 同源同理解，
   两者对得上只能证明【自洽】，不能证明【符合规约】。规约符合性由
   test/dlt_frame_test 的字节级单元测试（基于规约示例帧）保证。

端口：
  20108  DLT645  请求/响应
  20109  DLT698  建链 + GET

仅覆盖 TCP 路径；SerialTransport(termios) 不在此验证范围内。
"""
import socket
import struct
import threading


# ─────────────────────────── 公共 socket 工具 ───────────────────────────
def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def serve(port, handler, name):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(5)
    print(f"[{name}] TCP 模拟器启动 :{port}", flush=True)
    while True:
        conn, addr = s.accept()
        threading.Thread(target=handler, args=(conn,), daemon=True).start()


# ═══════════════════════════ DL/T 645-2007 ═══════════════════════════
# 帧: 0x68 ADDR[6] 0x68 CTRL LEN DATA(+0x33...) CS 0x16
# 读响应 CTRL=0x91；DATA = DI[4] + 值字节，全部 +0x33；BCD 为 LSB-first

# DI(十六进制字符串) → (原始整数, BCD字节数)。采集器再乘 config.scale。
DLT645_VALUES = {
    0x00010000: (12345, 4),   # 总电能 raw=12345  scale0.01 → 123.45 kWh
    0x02010100: (2201,  2),   # A相电压 raw=2201  scale0.1  → 220.1 V
    0x02020100: (1502,  3),   # A相电流 raw=1502  scale0.001→ 1.502 A
    0x02030000: (12345, 3),   # 有功功率 raw=12345 scale0.0001→1.2345 kW
    0x02060000: (999,   2),   # 功率因数 raw=999  scale0.001 → 0.999
}


def int_to_bcd_le(value, nbytes):
    """整数 → BCD 字节（LSB-first），与采集器 bcdToDouble 互逆"""
    out = []
    v = int(round(value))
    for _ in range(nbytes):
        lo = v % 10; v //= 10
        hi = v % 10; v //= 10
        out.append((hi << 4) | lo)
    return out


def dlt645_handler(conn):
    try:
        while True:
            # 跳过前导 0xFE / 噪声，定位首个 0x68
            b = recv_exact(conn, 1)
            if b is None:
                return
            if b[0] != 0x68:
                continue
            hdr = recv_exact(conn, 9)   # ADDR[6] 0x68 CTRL LEN
            if hdr is None:
                return
            addr = hdr[0:6]
            ctrl, length = hdr[7], hdr[8]
            rest = recv_exact(conn, length + 2)   # DATA + CS + 0x16
            if rest is None:
                return
            data = rest[:length]

            # 解码 DI（前4字节 -0x33，LSB-first）
            di_bytes = [(x - 0x33) & 0xFF for x in data[:4]]
            di = di_bytes[0] | (di_bytes[1] << 8) | (di_bytes[2] << 16) | (di_bytes[3] << 24)

            raw, nbytes = DLT645_VALUES.get(di, (0, 4))
            val_bytes = int_to_bcd_le(raw, nbytes)

            # 响应 DATA = DI(echo) + 值字节，全部 +0x33
            resp_data = [(x + 0x33) & 0xFF for x in di_bytes] + \
                        [(x + 0x33) & 0xFF for x in val_bytes]
            resp_len = len(resp_data)

            frame = [0x68] + list(addr) + [0x68, 0x91, resp_len] + resp_data
            cs = sum(frame) & 0xFF
            frame += [cs, 0x16]
            conn.sendall(bytes(frame))
    except (ConnectionError, OSError):
        pass
    finally:
        conn.close()


# ═══════════════════════════ DL/T 698.45 ═══════════════════════════
# 帧: 0x68 LEN(2,LE) INNER CRC(2,LE) 0x16
# INNER: CTRL SER DSALEN DSA SSALEN SSA APPDU
# CRC-16/ARC: init=0x0000 poly=0xA001(reflected)，覆盖 LEN+INNER

# OAD(十六进制) → float32 物理值（698 config scale 设为 1.0）
DLT698_VALUES = {
    0x00200200: 123.45,   # 总电能 kWh
    0x02010100: 220.1,    # A相电压 V
    0x02040100: 1.502,    # A相电流 A
    0x03010100: 1.2345,   # 有功功率 kW
}


def crc16_arc(data):
    crc = 0x0000
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF


def dlt698_build_frame(inner):
    length = len(inner)
    crc_input = bytes([length & 0xFF, (length >> 8) & 0xFF]) + bytes(inner)
    crc = crc16_arc(crc_input)
    return bytes([0x68, length & 0xFF, (length >> 8) & 0xFF]) + bytes(inner) + \
        bytes([crc & 0xFF, (crc >> 8) & 0xFF, 0x16])


def dlt698_handler(conn):
    try:
        while True:
            b = recv_exact(conn, 1)
            if b is None:
                return
            if b[0] != 0x68:
                continue
            lb = recv_exact(conn, 2)
            if lb is None:
                return
            inner_len = lb[0] | (lb[1] << 8)
            body = recv_exact(conn, inner_len + 3)  # INNER + CRC(2) + 0x16
            if body is None:
                return
            inner = body[:inner_len]

            # 解析 INNER：CTRL SER DSALEN DSA SSALEN SSA APPDU
            p = 2                       # 跳过 CTRL, SER
            dsa_len = inner[p]; p += 1 + dsa_len
            ssa_len = inner[p]; p += 1 + ssa_len
            appdu = inner[p:]
            if not appdu:
                continue
            svc = appdu[0]

            if svc == 0x40:             # 建立应用连接请求
                piid = appdu[1] if len(appdu) > 1 else 0
                resp_appdu = [0xC0, piid, 0x00]
            elif svc == 0x01:           # GET-Request-Normal
                piid = appdu[1]
                oad = appdu[2:6]
                oad_val = (oad[0] << 24) | (oad[1] << 16) | (oad[2] << 8) | oad[3]
                value = DLT698_VALUES.get(oad_val, 0.0)
                # 类型 0x06 = float32 (LE)；APPDU: 0x81 PIID OAD[4] AEN(0) tag value
                fbytes = struct.pack("<f", float(value))
                resp_appdu = [0x81, piid] + list(oad) + [0x00, 0x06] + list(fbytes)
            else:
                continue

            # 回复 INNER：CTRL SER DSALEN DSA(主站零地址) SSALEN SSA(表地址回显)
            ser = appdu[1] if len(appdu) > 1 else 0
            resp_inner = [0x00, ser, 0x06] + [0]*6 + [0x06] + [0]*6 + resp_appdu
            conn.sendall(dlt698_build_frame(resp_inner))
    except (ConnectionError, OSError):
        pass
    finally:
        conn.close()


def run():
    threading.Thread(target=serve, args=(20108, dlt645_handler, "dlt645"),
                     daemon=True).start()
    serve(20109, dlt698_handler, "dlt698")


if __name__ == "__main__":
    run()
