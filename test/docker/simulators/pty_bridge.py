"""
虚拟串口桥：把一个 PTY 接到已有的 DLT645 TCP 模拟器上。

为什么需要它
────────────
`dlt_sim.py` 只监听 TCP，覆盖的是采集器的 TcpTransport。而 DLT645 在现场多数是
RS485 直连，走的是 SerialTransport（termios）—— 那条路径此前【一次都没跑过】。
本桥开一个 PTY，把从从设备端收到的字节原样转给 TCP 模拟器，再把回帧写回去。
于是同一个模拟器、同一批帧，能同时验证两种传输。

它不是另写一个模拟器 —— 协议逻辑仍然只有 dlt_sim.py 一份。

覆盖到什么 / 覆盖不到什么
────────────────────────
覆盖：open(O_NONBLOCK) / tcgetattr / tcsetattr / tcflush / tcdrain / poll 读超时，
      以及 writeAll、readDeadline 的 socketMode=false 分支。
覆盖不到：真实波特率（在 PTY 上 cfsetispeed 会被接受但没有意义）、RS485 半双工
      收发切换时序、校验/帧错误。所以只能说"termios 调用路径已覆盖"，
      不能说"串口路径已覆盖"。

用法
────
    python3 pty_bridge.py [--tcp 127.0.0.1:15644] [--link /tmp/ic_ttyDLT645]

前台运行，Ctrl-C 退出；退出时删掉软链接。
"""
import argparse
import errno
import os
import pty
import select
import signal
import socket
import sys
import termios
import tty


def make_raw(fd):
    """把从设备端设成 raw。

    采集器自己在 open() 里也会设 raw，但那是【它连上之后】的事。在此之前 PTY
    默认是带回显的行规程，任何先到的字节会被原样弹回去 —— 采集器随后就会把
    自己发出的请求当成应答帧读进来，症状是校验和莫名其妙地对不上。
    """
    attrs = termios.tcgetattr(fd)
    tty.setraw(fd, termios.TCSANOW)
    return attrs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tcp",  default="127.0.0.1:15644",
                    help="DLT645 TCP 模拟器地址（默认 127.0.0.1:15644）")
    ap.add_argument("--link", default="/tmp/ic_ttyDLT645",
                    help="给 PTY 建的稳定软链接路径")
    args = ap.parse_args()

    host, _, port = args.tcp.partition(":")
    port = int(port)

    master, slave = pty.openpty()
    make_raw(slave)
    ttyname = os.ttyname(slave)

    # 【从设备端 fd 全程不关】。一旦关掉，采集器断开的那一刻 PTY 就被拆掉，
    # 软链接随即悬空；而且主设备端 read() 会在没有从设备打开时不停返回 EIO，
    # select 立刻可读，循环空转烧 CPU。留着它，read() 才会老实阻塞。
    # 但绝不从这个 fd 读 —— 那会和采集器抢同一份数据。

    if os.path.islink(args.link) or os.path.exists(args.link):
        os.unlink(args.link)
    os.symlink(ttyname, args.link)

    sock = socket.create_connection((host, port), timeout=5)
    sock.setblocking(False)

    print(f"[pty_bridge] {args.link} → {ttyname}  ⇄  {host}:{port}", flush=True)
    print(f"[pty_bridge] 采集器把 serial_port 指到 {args.link} 即可", flush=True)

    stop = False

    def on_sig(_s, _f):
        nonlocal stop
        stop = True
    signal.signal(signal.SIGINT,  on_sig)
    signal.signal(signal.SIGTERM, on_sig)

    try:
        while not stop:
            try:
                r, _, _ = select.select([master, sock], [], [], 0.5)
            except (InterruptedError, OSError):
                continue

            if master in r:
                try:
                    data = os.read(master, 4096)
                except OSError as e:
                    # EIO = 此刻没有别的进程打开从设备端（采集器正在重连）。
                    # 我们自己扣着 slave，正常不该走到这里，兜底忽略即可。
                    if e.errno == errno.EIO:
                        continue
                    raise
                if data:
                    sock.sendall(data)

            if sock in r:
                try:
                    data = sock.recv(4096)
                except BlockingIOError:
                    continue
                if not data:
                    print("[pty_bridge] TCP 模拟器断开，退出", flush=True)
                    break
                os.write(master, data)
    finally:
        try:
            if os.path.islink(args.link):
                os.unlink(args.link)
        except OSError:
            pass
        sock.close()
        os.close(master)
        os.close(slave)
        print("[pty_bridge] 已退出", flush=True)


if __name__ == "__main__":
    sys.exit(main())
