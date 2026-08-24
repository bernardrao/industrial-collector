#!/bin/bash
# 生成 Web HTTPS 自签证书（cert.pem / key.pem）。
# 用 third_party 里静态编译出的 openssl，无需系统 openssl。
#
# 用法：
#   bash scripts/gen_cert.sh [输出目录] [CN]
#   默认输出到当前目录，CN=industrial-collector
# 然后在 config.json 的 web 段设置：
#   "tls_enabled": true, "tls_cert": "cert.pem", "tls_key": "key.pem"
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${1:-.}"
CN="${2:-industrial-collector}"

OPENSSL="$ROOT/third_party/install/openssl/bin/openssl"
[[ -x "$OPENSSL" ]] || OPENSSL="$(command -v openssl)"
[[ -n "$OPENSSL" ]] || { echo "找不到 openssl，请先运行 scripts/bootstrap.sh"; exit 1; }

mkdir -p "$OUT"
# 静态编译的 openssl 未安装默认 openssl.cnf，自带一个最小配置
CNF="$(mktemp)"
cat > "$CNF" <<EOF
[req]
distinguished_name = dn
x509_extensions = v3
prompt = no
[dn]
CN = $CN
[v3]
subjectAltName = DNS:localhost, IP:127.0.0.1
basicConstraints = critical, CA:TRUE
EOF
"$OPENSSL" req -x509 -newkey rsa:2048 -nodes -config "$CNF" \
    -keyout "$OUT/key.pem" -out "$OUT/cert.pem" -days 3650
rm -f "$CNF"

echo "已生成: $OUT/cert.pem  $OUT/key.pem  (CN=$CN, 有效期 10 年)"
echo "在 config.json web 段启用: \"tls_enabled\": true"
