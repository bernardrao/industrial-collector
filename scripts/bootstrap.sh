#!/bin/bash
# =============================================================================
# scripts/bootstrap.sh  —  Ubuntu 18.04 一键编译
#
# 支持协议: Modbus TCP / IEC 60870-5-104 / IEC 61850 MMS / OPC UA / DLT645 / DLT698
# DLT645/DLT698 使用 POSIX 串口/TCP，无额外依赖，无需额外编译步骤。
#
# 依赖库（全部从源码静态编译，不依赖系统库）:
#   nlohmann/json  v3.11.3  header-only
#   cpp-httplib    v0.12.6  header-only（锁定此版本，v0.13+需要 OpenSSL 3）
#   spdlog         v1.13.0  header-only
#   libmodbus      v3.1.10  静态库
#   paho.mqtt.c    v1.3.13  静态库（NO SSL）
#   paho.mqtt.cpp  v1.3.2   静态库（NO SSL）
#   libiec61850    v1.5.3   静态库
#   open62541      v1.3.9   静态库（amalgamation 单文件）
#
# 用法:
#   bash scripts/bootstrap.sh           # 首次运行
#   bash scripts/bootstrap.sh --clean   # 清除 build/ 重编
# =============================================================================
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
BUILD="$ROOT/build"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
info() { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
die()  { echo -e "${RED}[FAIL]${NC}  $*"; exit 1; }
step() { echo -e "\n${GREEN}══${NC} $* ${GREEN}══${NC}"; }

[[ "$1" == "--clean" ]] && { info "清除 build/..."; rm -rf "$BUILD"; }
mkdir -p "$TP"

# ── 第三方库编译输出统一目录（不再散落各处）──────────────────────────────────
#   $TP/install/<lib>/{lib,include}  各库独立 prefix（绝不合并 include，
#                                    避免 libiec61850 等的标准头垫片冲突）
#   $TP/build/<lib>/                 中间构建树（cmake/autotools 树外编译）
# 源码 *-src/ 与 header-only 库（nlohmann/httplib/spdlog）仍留在 $TP 顶层。
TP_INSTALL="$TP/install"
TP_BUILD="$TP/build"
mkdir -p "$TP_INSTALL" "$TP_BUILD"

# ── 系统基础包 ────────────────────────────────────────────────────────────────
step "1/10  安装系统依赖"
# apt-get update 失败（如某个无关第三方源 GPG/404）不应中止整个编译：
# 用缓存索引继续，缺包会在 install 阶段暴露。
sudo apt-get update -qq || warn "apt-get update 有源失败，使用缓存索引继续"
sudo apt-get install -y --no-install-recommends \
    build-essential cmake git wget curl ca-certificates unzip perl \
    pkg-config gcc-9 g++-9 autoconf libtool python3 \
    || warn "apt-get install 失败：若所需工具已安装可忽略，否则请手动安装"
# 不安装 libssl-dev：所有组件以 NO-SSL 模式编译

# gcc-9 为默认
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-9 80 \
     --slave /usr/bin/g++ g++ /usr/bin/g++-9 2>/dev/null || true
info "GCC: $(gcc --version | head -1)"

# CMake >= 3.14
CMAKE_OK=0
if command -v cmake &>/dev/null; then
    CMAJ=$(cmake --version | grep -oP '\d+' | head -1)
    CMIN=$(cmake --version | grep -oP '\d+' | sed -n '2p')
    [[ "$CMAJ" -gt 3 || ( "$CMAJ" -eq 3 && "$CMIN" -ge 14 ) ]] && CMAKE_OK=1
fi
if [[ "$CMAKE_OK" -eq 0 ]]; then
    step "   升级 CMake"
    wget -q --show-progress \
        "https://github.com/Kitware/CMake/releases/download/v3.27.9/cmake-3.27.9-linux-x86_64.tar.gz" \
        -O /tmp/cmake.tar.gz
    sudo tar xf /tmp/cmake.tar.gz -C /usr/local --strip-components=1
    rm -f /tmp/cmake.tar.gz
fi
info "CMake: $(cmake --version | head -1)"

# ── nlohmann/json v3.11.3 ─────────────────────────────────────────────────────
step "2/10  nlohmann/json v3.11.3"
JSON_H="$TP/nlohmann/json.hpp"
if [[ ! -f "$JSON_H" ]]; then
    mkdir -p "$TP/nlohmann"
    wget -q --show-progress \
        "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" \
        -O "$JSON_H" 2>/dev/null || {
        wget -q "https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz" \
             -O /tmp/json.tar.gz
        tar xf /tmp/json.tar.gz -C /tmp
        cp /tmp/json-3.11.3/single_include/nlohmann/json.hpp "$JSON_H"
        rm -rf /tmp/json.tar.gz /tmp/json-3.11.3
    }
    [[ -f "$JSON_H" ]] || die "nlohmann/json 下载失败"
    info "OK"
else
    info "已存在，跳过"
fi

# ── cpp-httplib v0.12.6（锁定，v0.13+ 需要 OpenSSL 3）────────────────────────
step "3/10  cpp-httplib v0.12.6"
HTTPLIB_H="$TP/httplib.h"
if [[ ! -f "$HTTPLIB_H" ]]; then
    wget -q --show-progress \
        "https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.12.6.tar.gz" \
        -O /tmp/httplib.tar.gz || die "cpp-httplib 下载失败"
    HPATH=$(tar tf /tmp/httplib.tar.gz | grep '/httplib\.h$' | head -1)
    [[ -n "$HPATH" ]] || die "tarball 中找不到 httplib.h"
    tar xf /tmp/httplib.tar.gz -C /tmp "$HPATH"
    cp "/tmp/$HPATH" "$HTTPLIB_H"
    rm -f /tmp/httplib.tar.gz
    rm -rf "/tmp/$(echo "$HPATH" | cut -d/ -f1)"
    VER=$(grep -m1 'CPPHTTPLIB_VERSION' "$HTTPLIB_H" | grep -oP '"[^"]+"' || echo "unknown")
    info "版本: $VER"
else
    info "已存在，跳过"
fi

# ── spdlog v1.13.0 ────────────────────────────────────────────────────────────
step "4/10  spdlog v1.13.0"
SPDLOG_H="$TP/spdlog/include/spdlog/spdlog.h"
if [[ ! -f "$SPDLOG_H" ]]; then
    wget -q --show-progress \
        "https://github.com/gabime/spdlog/archive/refs/tags/v1.13.0.tar.gz" \
        -O /tmp/spdlog.tar.gz || die "spdlog 下载失败"
    mkdir -p "$TP/spdlog"
    tar xf /tmp/spdlog.tar.gz -C "$TP/spdlog" --strip-components=1
    rm -f /tmp/spdlog.tar.gz
    info "OK"
else
    info "已存在，跳过"
fi

# ── libmodbus v3.1.10 ─────────────────────────────────────────────────────────
step "5/10  libmodbus v3.1.10"
MODBUS_INST="$TP_INSTALL/libmodbus"
if [[ ! -f "$MODBUS_INST/lib/libmodbus.a" ]]; then
    MODBUS_SRC="$TP/libmodbus-src"
    if [[ ! -d "$MODBUS_SRC" ]]; then
        wget -q --show-progress \
            "https://github.com/stephane/libmodbus/archive/refs/tags/v3.1.10.tar.gz" \
            -O /tmp/libmodbus.tar.gz || die "libmodbus 下载失败"
        mkdir -p "$MODBUS_SRC"
        tar xf /tmp/libmodbus.tar.gz -C "$MODBUS_SRC" --strip-components=1
        rm -f /tmp/libmodbus.tar.gz
    fi
    # 清理源码树内可能的旧 in-tree configure 残留（否则 VPATH configure 会报
    # "source directory already configured"），再 autoreconf 生成 configure。
    [[ -f "$MODBUS_SRC/Makefile" ]] && make -C "$MODBUS_SRC" distclean >/dev/null 2>&1 || true
    rm -f "$MODBUS_SRC/config.status" "$MODBUS_SRC/config.log"
    ( cd "$MODBUS_SRC" && autoreconf -fis ) 2>&1 | tail -3
    # VPATH：在独立 build 目录树外编译
    mkdir -p "$TP_BUILD/libmodbus" && cd "$TP_BUILD/libmodbus"
    "$MODBUS_SRC/configure" --prefix="$MODBUS_INST" \
        --enable-static --disable-shared --disable-tests \
        CC=gcc-9 CFLAGS="-O2 -fPIC" 2>&1 | tail -3
    make -j"$(nproc)" 2>&1 | tail -2
    make install 2>&1 | tail -1
    [[ -f "$MODBUS_INST/lib/libmodbus.a" ]] || die "libmodbus 编译失败"
    cd "$ROOT"
    info "OK: $MODBUS_INST/lib/libmodbus.a"
else
    info "已存在，跳过"
fi

# ── paho.mqtt.c v1.3.13 ───────────────────────────────────────────────────────
step "6/10  paho.mqtt.c v1.3.13"
PAHOC_INST="$TP_INSTALL/paho-c"
if [[ ! -f "$PAHOC_INST/lib/libpaho-mqtt3a.a" ]]; then
    PAHOC_SRC="$TP/paho-c-src"
    if [[ ! -d "$PAHOC_SRC" ]]; then
        wget -q --show-progress \
            "https://github.com/eclipse/paho.mqtt.c/archive/refs/tags/v1.3.13.tar.gz" \
            -O /tmp/pahoc.tar.gz || die "paho.mqtt.c 下载失败"
        mkdir -p "$PAHOC_SRC"
        tar xf /tmp/pahoc.tar.gz -C "$PAHOC_SRC" --strip-components=1
        rm -f /tmp/pahoc.tar.gz
    fi
    mkdir -p "$TP_BUILD/paho-c" && cd "$TP_BUILD/paho-c"
    cmake "$PAHOC_SRC" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PAHOC_INST" \
        -DCMAKE_C_COMPILER=gcc-9 -DCMAKE_C_FLAGS="-fPIC" \
        -DPAHO_BUILD_STATIC=ON -DPAHO_BUILD_SHARED=OFF \
        -DPAHO_WITH_SSL=OFF -DPAHO_ENABLE_TESTING=OFF \
        -DPAHO_BUILD_SAMPLES=OFF 2>&1 | tail -4
    make -j"$(nproc)" 2>&1 | tail -2
    make install 2>&1 | tail -1
    cd "$ROOT"
    # paho 自带 SHA1（WebSocket 握手用）与 OpenSSL libcrypto 的 SHA1 同名且 SHA_CTX
    # 结构不同，链接 HTTPS 时会符号冲突。重命名 paho 的 SHA1 符号（def+ref 一致），
    # 我们走 tcp:// 不用 WebSocket，运行时不调用，重命名安全。
    for A in "$PAHOC_INST/lib/libpaho-mqtt3a.a" "$PAHOC_INST/lib/libpaho-mqtt3c.a"; do
        [[ -f "$A" ]] && objcopy \
            --redefine-sym SHA1_Init=paho_SHA1_Init \
            --redefine-sym SHA1_Update=paho_SHA1_Update \
            --redefine-sym SHA1_Final=paho_SHA1_Final \
            --redefine-sym SHA1_Transform=paho_SHA1_Transform "$A" || true
    done
    info "OK"
else
    info "已存在，跳过"
fi

# ── paho.mqtt.cpp v1.3.2 ──────────────────────────────────────────────────────
step "7/10  paho.mqtt.cpp v1.3.2"
PAHOPP_INST="$TP_INSTALL/paho-cpp"
if [[ ! -f "$PAHOPP_INST/lib/libpaho-mqttpp3.a" ]]; then
    PAHOPP_SRC="$TP/paho-cpp-src"
    if [[ ! -d "$PAHOPP_SRC" ]]; then
        wget -q --show-progress \
            "https://github.com/eclipse/paho.mqtt.cpp/archive/refs/tags/v1.3.2.tar.gz" \
            -O /tmp/pahopp.tar.gz || die "paho.mqtt.cpp 下载失败"
        mkdir -p "$PAHOPP_SRC"
        tar xf /tmp/pahopp.tar.gz -C "$PAHOPP_SRC" --strip-components=1
        rm -f /tmp/pahopp.tar.gz
    fi
    mkdir -p "$TP_BUILD/paho-cpp" && cd "$TP_BUILD/paho-cpp"
    cmake "$PAHOPP_SRC" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PAHOPP_INST" \
        -DCMAKE_CXX_COMPILER=g++-9 -DCMAKE_CXX_FLAGS="-fPIC" \
        -DPAHO_BUILD_STATIC=ON -DPAHO_BUILD_SHARED=OFF \
        -DPAHO_WITH_SSL=OFF \
        -DPAHO_MQTT_C_LIBRARIES="$PAHOC_INST/lib/libpaho-mqtt3a.a" \
        -DPAHO_MQTT_C_INCLUDE_DIRS="$PAHOC_INST/include" \
        -DPAHO_BUILD_SAMPLES=OFF -DPAHO_BUILD_DOCUMENTATION=OFF \
        2>&1 | tail -4
    make -j"$(nproc)" 2>&1 | tail -2
    make install 2>&1 | tail -1
    cd "$ROOT"
    info "OK"
else
    info "已存在，跳过"
fi

# ── libiec61850 v1.5.3 ────────────────────────────────────────────────────────
step "8/10  libiec61850 v1.5.3"
IEC61850_INST="$TP_INSTALL/libiec61850"
if [[ ! -f "$IEC61850_INST/lib/libiec61850.a" ]]; then
    IEC61850_SRC="$TP/libiec61850-src"
    if [[ ! -d "$IEC61850_SRC" ]]; then
        info "下载 libiec61850..."
        wget -q --show-progress \
            "https://github.com/mz-automation/libiec61850/archive/refs/tags/v1.5.3.tar.gz" \
            -O /tmp/iec61850.tar.gz || die "libiec61850 下载失败"
        mkdir -p "$IEC61850_SRC"
        tar xf /tmp/iec61850.tar.gz -C "$IEC61850_SRC" --strip-components=1
        rm -f /tmp/iec61850.tar.gz
    fi
    info "编译 libiec61850..."
    mkdir -p "$TP_BUILD/libiec61850" && cd "$TP_BUILD/libiec61850"
    cmake "$IEC61850_SRC" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$IEC61850_INST" \
        -DCMAKE_C_COMPILER=gcc-9 \
        -DCMAKE_C_FLAGS="-O2 -fPIC" \
        -DBUILD_SHARED_LIBS=OFF \
        -DUSE_MBEDTLS=OFF \
        -DUSE_OPENSSL=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_TESTS=OFF \
        2>&1 | tail -5
    make -j"$(nproc)" iec61850 2>&1 | tail -2
    # 手动安装头文件和静态库（libiec61850 的 install target 路径不一致）
    mkdir -p "$IEC61850_INST/lib" "$IEC61850_INST/include"
    find . -name "libiec61850.a" -exec cp {} "$IEC61850_INST/lib/" \;
    find . -name "libhal.a"      -exec cp {} "$IEC61850_INST/lib/" \;
    # 头文件：从源码树复制
    # 从源码树各子目录拷贝全部头文件（含 HAL 平台头文件）
    find "$IEC61850_SRC/src"  -name "*.h" -exec cp {} "$IEC61850_INST/include/" \; 2>/dev/null || true
    find "$IEC61850_SRC/hal"  -name "*.h" -exec cp {} "$IEC61850_INST/include/" \; 2>/dev/null || true
    # build 目录中的生成头文件
    find . -name "*.h" -path "*/config/*" -exec cp {} "$IEC61850_INST/include/" \; 2>/dev/null || true
    # 删除 libiec61850 给 MSVC 的标准头垫片：它们会随 -isystem 覆盖系统标准头。
    # 尤其 stdbool.h 内含 "#define bool int"，会污染 open62541.h 的 C++ 编译。
    rm -f "$IEC61850_INST/include/stdbool.h" \
          "$IEC61850_INST/include/stdint.h" \
          "$IEC61850_INST/include/inttypes.h"
    cd "$ROOT"
    [[ -f "$IEC61850_INST/lib/libiec61850.a" ]] || die "libiec61850 编译失败"
    info "OK: $IEC61850_INST/lib/libiec61850.a"
else
    info "已存在，跳过"
fi

# ── open62541 v1.3.9（OPC UA，amalgamation 单文件）────────────────────────────
step "9/10  open62541 v1.3.9"
UA_INST="$TP_INSTALL/open62541"
if [[ ! -f "$UA_INST/include/open62541/open62541.h" ]]; then
    UA_SRC="$TP/open62541-src"
    if [[ ! -d "$UA_SRC" ]]; then
        info "下载 open62541..."
        wget -q --show-progress \
            "https://github.com/open62541/open62541/archive/refs/tags/v1.3.9.tar.gz" \
            -O /tmp/open62541.tar.gz || die "open62541 下载失败"
        mkdir -p "$UA_SRC"
        tar xf /tmp/open62541.tar.gz -C "$UA_SRC" --strip-components=1
        rm -f /tmp/open62541.tar.gz
    fi
    info "编译 open62541（amalgamation 模式，单 .c 文件，约 3~5 分钟）..."
    mkdir -p "$TP_BUILD/open62541" && cd "$TP_BUILD/open62541"
    cmake "$UA_SRC" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$UA_INST" \
        -DCMAKE_C_COMPILER=gcc-9 \
        -DCMAKE_CXX_COMPILER=g++-9 \
        -DCMAKE_C_FLAGS="-O2 -fPIC -Wno-error" \
        -DUA_BUILD_AMALGAMATION=ON \
        -DUA_ENABLE_AMALGAMATION=ON \
        -DUA_ENABLE_ENCRYPTION=OFF \
        -DUA_ENABLE_ENCRYPTION_MBEDTLS=OFF \
        -DUA_ENABLE_ENCRYPTION_OPENSSL=OFF \
        -DUA_ENABLE_PUBSUB=OFF \
        -DUA_ENABLE_HISTORIZING=OFF \
        -DUA_ENABLE_SUBSCRIPTIONS=ON \
        -DUA_NAMESPACE_ZERO=REDUCED \
        -DBUILD_SHARED_LIBS=OFF \
        -DUA_BUILD_EXAMPLES=OFF \
        -DUA_BUILD_UNIT_TESTS=OFF \
        2>&1 | tail -6
    make -j"$(nproc)" open62541-amalgamation-source open62541-amalgamation-header 2>/dev/null || \
    make -j"$(nproc)" 2>&1 | tail -4

    # 安装 amalgamation 文件
    mkdir -p "$UA_INST/include/open62541" "$UA_INST/lib" "$UA_INST/src"

    # 找 amalgamation 头文件
    UA_H=$(find . -name "open62541.h" | head -1)
    UA_C=$(find . -name "open62541.c" | head -1)

    [[ -n "$UA_H" ]] || die "找不到 open62541.h amalgamation 文件"
    [[ -n "$UA_C" ]] || die "找不到 open62541.c amalgamation 文件"

    cp "$UA_H" "$UA_INST/include/open62541/open62541.h"
    cp "$UA_C" "$UA_INST/src/open62541.c"

    # 预编译为静态库（避免每次重编译 amalgamation）
    info "预编译 open62541 静态库..."
    gcc-9 -O2 -fPIC -std=c99 \
        -I"$UA_INST/include" \
        -I"$UA_INST/include/open62541" \
        -c "$UA_INST/src/open62541.c" \
        -o "$UA_INST/lib/open62541.o" \
        -Wno-unused-parameter -Wno-unused-function \
        -Wno-format -Wno-strict-aliasing 2>&1 | tail -3
    ar rcs "$UA_INST/lib/libopen62541.a" "$UA_INST/lib/open62541.o"
    rm -f "$UA_INST/lib/open62541.o"

    cd "$ROOT"
    [[ -f "$UA_INST/lib/libopen62541.a" ]] || die "open62541 编译失败"
    info "OK: $UA_INST/lib/libopen62541.a"
else
    info "已存在，跳过"
fi

# ── SQLite（amalgamation 单文件，本地缓存/断点续传用）────────────────────────
step "10/11 SQLite amalgamation"
SQLITE_INST="$TP_INSTALL/sqlite3"
SQLITE_VER="3450300"   # 3.45.3
if [[ ! -f "$SQLITE_INST/lib/libsqlite3.a" ]]; then
    SQLITE_SRC="$TP/sqlite-src"
    if [[ ! -f "$SQLITE_SRC/sqlite3.c" ]]; then
        info "下载 sqlite amalgamation..."
        wget -q --show-progress \
            "https://www.sqlite.org/2024/sqlite-amalgamation-${SQLITE_VER}.zip" \
            -O /tmp/sqlite.zip || die "sqlite 下载失败"
        mkdir -p "$SQLITE_SRC"
        ( cd /tmp && unzip -oq sqlite.zip && \
          cp "sqlite-amalgamation-${SQLITE_VER}"/sqlite3.{c,h} "$SQLITE_SRC/" )
        rm -rf "/tmp/sqlite-amalgamation-${SQLITE_VER}" /tmp/sqlite.zip
        [[ -f "$SQLITE_SRC/sqlite3.c" ]] || die "sqlite amalgamation 解压失败"
    fi
    info "预编译 sqlite 静态库..."
    mkdir -p "$SQLITE_INST/lib" "$SQLITE_INST/include"
    cp "$SQLITE_SRC/sqlite3.h" "$SQLITE_INST/include/"
    gcc-9 -O2 -fPIC -c "$SQLITE_SRC/sqlite3.c" -o "$TP_BUILD/sqlite3.o" \
        -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION 2>&1 | tail -3
    ar rcs "$SQLITE_INST/lib/libsqlite3.a" "$TP_BUILD/sqlite3.o"
    rm -f "$TP_BUILD/sqlite3.o"
    [[ -f "$SQLITE_INST/lib/libsqlite3.a" ]] || die "sqlite 编译失败"
    info "OK: $SQLITE_INST/lib/libsqlite3.a"
else
    info "已存在，跳过"
fi

# ── OpenSSL 1.1.1w（静态，Web HTTPS 用；httplib v0.12.6 要求 >=1.1.1）────────
step "11/12 OpenSSL 1.1.1w"
SSL_INST="$TP_INSTALL/openssl"
SSL_VER="1.1.1w"
if [[ ! -f "$SSL_INST/lib/libssl.a" ]]; then
    SSL_SRC="$TP/openssl-src"
    if [[ ! -f "$SSL_SRC/Configure" ]]; then
        info "下载 openssl $SSL_VER..."
        wget -q --show-progress \
            "https://www.openssl.org/source/openssl-${SSL_VER}.tar.gz" \
            -O /tmp/openssl.tar.gz \
        || wget -q --show-progress \
            "https://github.com/openssl/openssl/releases/download/OpenSSL_${SSL_VER//./_}/openssl-${SSL_VER}.tar.gz" \
            -O /tmp/openssl.tar.gz \
        || die "openssl 下载失败"
        mkdir -p "$SSL_SRC"
        tar xf /tmp/openssl.tar.gz -C "$SSL_SRC" --strip-components=1
        rm -f /tmp/openssl.tar.gz
    fi
    info "编译 openssl（静态，约 3~5 分钟）..."
    cd "$SSL_SRC"
    # OpenSSL 用自带 Configure（非 autotools），就地编译；no-shared 不会自动加 -fPIC，需显式
    make clean >/dev/null 2>&1 || true
    ./config no-shared no-tests -fPIC --prefix="$SSL_INST" CC=gcc-9 2>&1 | tail -3
    make -j"$(nproc)" 2>&1 | tail -2
    make install_sw 2>&1 | tail -2     # install_sw：仅库+头，跳过 man（免 perl POD 模块）
    cd "$ROOT"
    [[ -f "$SSL_INST/lib/libssl.a" && -f "$SSL_INST/lib/libcrypto.a" ]] \
        || die "openssl 编译失败"
    info "OK: $SSL_INST/lib/{libssl.a,libcrypto.a}"
else
    info "已存在，跳过"
fi

# ── 编译主项目 ────────────────────────────────────────────────────────────────
step "12/12 编译 industrial_collector"
mkdir -p "$BUILD" && cd "$BUILD"
cmake "$ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++-9 \
    -DTHIRD_PARTY_DIR="$TP" \
    2>&1 | tail -10
make -j"$(nproc)"
cd "$ROOT"

echo ""
echo -e "${GREEN}╔══════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  编译成功！                                       ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════╝${NC}"
echo ""
info "可执行文件:  $BUILD/industrial_collector"
info "修改配置:    nano config/config.json"
info "  设置 \"protocol\": \"modbus\"   | \"iec104\" | \"iec61850\" | \"opcua\""
echo ""
info "运行:"
echo "    ./build/industrial_collector config/config.json"
echo ""
info "Web 管理界面:  http://<设备IP>:8080"
echo ""
