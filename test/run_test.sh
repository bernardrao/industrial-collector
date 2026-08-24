#!/bin/bash
# =============================================================================
# test/run_test.sh  —  一键测试：编译 + 起模拟器 + 跑采集器
#
# 覆盖六协议端到端：Modbus / IEC104 / IEC61850 / OPC UA / DLT645 / DLT698
# 采集器跑在【宿主机】，MQTT broker 与协议模拟器跑在 Docker，IEC61850
# 测试服务端用 libiec61850 自带 server_example 在宿主机本地起。
#
# 用法：
#   bash test/run_test.sh build      # 仅编译（采集器 + IEC61850 服务端）
#   bash test/run_test.sh up         # 起 docker 模拟器 + IEC61850 服务端
#   bash test/run_test.sh run        # 前台运行采集器（Ctrl-C 停止）
#   bash test/run_test.sh sub        # 订阅 MQTT 查看实时数据
#   bash test/run_test.sh down       # 停止全部（docker + IEC61850 服务端）
#   bash test/run_test.sh all        # build + up + run（最常用）
#
# 端口配置位置见 test/TESTING.md。
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${IC_BUILD_DIR:-$ROOT/build}"
DOCKER_DIR="$HERE/docker"
CFG="$HERE/docker/config.docker.json" 
IED_PORT=15646
PTY_LINK=/tmp/ic_ttyDLT645       # 虚拟串口（DLT645 的 termios 路径）
PTY_PID=/tmp/ic_pty_bridge.pid

CC="${CC:-gcc-9}"
CXX="${CXX:-g++-9}"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
info(){ echo -e "${GREEN}[INFO]${NC} $*"; }
warn(){ echo -e "${YELLOW}[WARN]${NC} $*"; }
die(){  echo -e "${RED}[FAIL]${NC} $*"; exit 1; }

# docker compose v2 / v1 兼容
dc(){ if docker compose version &>/dev/null; then docker compose "$@"; else docker-compose "$@"; fi; }

do_build(){
    info "编译采集器（$CXX）…"
    mkdir -p "$BUILD"
    ( cd "$BUILD" && cmake "$ROOT" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
        -DTHIRD_PARTY_DIR="$ROOT/third_party" >/dev/null && make -j"$(nproc)" )
    [[ -x "$BUILD/industrial_collector" ]] || die "采集器编译失败"
    info "采集器：$BUILD/industrial_collector"

    info "编译 IEC61850 测试服务端…"
    CC="$CC" bash "$HERE/iec61850_server/build.sh" >/dev/null
    info "IEC61850 服务端：$HERE/iec61850_server/iec61850_server"

    info "（可选）DLT 字节级单元测试：cd build && make dlt_frame_test && ./dlt_frame_test"
}

do_up(){
    info "启动 Docker 模拟器 + MQTT broker…"
    ( cd "$DOCKER_DIR" && dc up -d --build )
    info "启动 IEC61850 服务端（:$IED_PORT）…"
    [[ -x "$HERE/iec61850_server/iec61850_server" ]] || die "IEC61850 服务端未编译，先 build"
    pkill -f "iec61850_server $IED_PORT" 2>/dev/null || true
    nohup "$HERE/iec61850_server/iec61850_server" "$IED_PORT" >/tmp/ic_ied.log 2>&1 &
    sleep 3

    # 虚拟串口桥：把 PTY 接到容器里那个 DLT645 TCP 模拟器上，让 dlt645_serial
    # 走 SerialTransport(termios)。不起它的话 config.docker.json 里的
    # dlt645_serial 会一直"重连中"，看着像采集器坏了。
    stop_pty
    info "启动虚拟串口桥（$PTY_LINK）…"
    nohup python3 "$DOCKER_DIR/simulators/pty_bridge.py" \
        --tcp 127.0.0.1:15644 --link "$PTY_LINK" >/tmp/ic_pty.log 2>&1 &
    echo $! > "$PTY_PID"
    for i in $(seq 1 30); do sleep 0.2; [[ -e "$PTY_LINK" ]] && break; done
    [[ -e "$PTY_LINK" ]] || { cat /tmp/ic_pty.log; die "虚拟串口桥未就绪"; }
    info "虚拟串口：$PTY_LINK → $(readlink "$PTY_LINK")"

    info "全部就绪。Web 界面: http://127.0.0.1:15647"
}

do_run(){
    [[ -x "$BUILD/industrial_collector" ]] || die "采集器未编译，先 build"
    info "前台运行采集器（Ctrl-C 停止）…"
    exec "$BUILD/industrial_collector" "$CFG" "--debug"
}

do_sub(){
    info "订阅 test/docker/#（Ctrl-C 退出）…"
    docker exec ic_test_mqtt mosquitto_sub -t 'test/docker/#' -v
}

# 只按 pid 文件停，且核对那真是我们起的桥 —— 不用 pkill -f：
# 发起命令的 shell 自己的命令行里就含着这个模式，会把自己一起打中。
stop_pty(){
    [[ -f "$PTY_PID" ]] || return 0
    local p; p=$(cat "$PTY_PID")
    if [[ -n "$p" ]] && grep -qs pty_bridge "/proc/$p/cmdline" 2>/dev/null; then
        kill "$p" 2>/dev/null || true
        for i in $(seq 1 25); do sleep 0.2; [[ -d /proc/$p ]] || break; done
    fi
    rm -f "$PTY_PID"
}

do_down(){
    info "停止虚拟串口桥…"
    stop_pty
    rm -f "$PTY_LINK"
    info "停止 IEC61850 服务端…"
    pkill -f "iec61850_server $IED_PORT" 2>/dev/null || true
    info "停止 Docker…"
    ( cd "$DOCKER_DIR" && dc down ) || true
    info "已全部停止。"
}

case "${1:-all}" in
    build) do_build ;;
    up)    do_up ;;
    run)   do_run ;;
    sub)   do_sub ;;
    down)  do_down ;;
    all)   do_build; do_up; do_run ;;
    *) echo "用法: $0 {build|up|run|sub|down|all}"; exit 1 ;;
esac
