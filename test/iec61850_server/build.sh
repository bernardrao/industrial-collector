#!/bin/bash
# 编译 libiec61850 自带的 server_example_basic_io 作为本机 IEC 61850 测试服务端。
# 依赖已由 scripts/bootstrap.sh 编好的静态库：
#   third_party/install/libiec61850/lib/{libiec61850.a, libhal.a}
#
# 用法：
#   bash test/iec61850_server/build.sh
#   ./test/iec61850_server/iec61850_server [port]    # 默认 102，建议非特权端口如 15646
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
INST="$ROOT/third_party/install/libiec61850"

CC="${CC:-gcc-9}"
[[ -f "$INST/lib/libiec61850.a" ]] || { echo "缺少 libiec61850.a，请先运行 scripts/bootstrap.sh"; exit 1; }

"$CC" -O2 \
    -I"$HERE" -I"$INST/include" \
    "$HERE/server_example_basic_io.c" "$HERE/static_model.c" \
    "$INST/lib/libiec61850.a" "$INST/lib/libhal.a" \
    -lpthread -lm \
    -o "$HERE/iec61850_server"

echo "OK: $HERE/iec61850_server"
echo "启动: $HERE/iec61850_server 15646   （非特权端口；102 需 root）"
