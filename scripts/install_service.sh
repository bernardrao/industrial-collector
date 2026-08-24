#!/bin/bash
# scripts/install_service.sh — 安装为 systemd 服务
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/industrial_collector"
CFG="$ROOT/config/config.json"

[ -f "$BIN" ] || { echo "请先编译: bash scripts/bootstrap.sh"; exit 1; }

sudo cp "$BIN" /usr/local/bin/industrial_collector
sudo mkdir -p /etc/industrial_collector
sudo cp "$CFG" /etc/industrial_collector/config.json

sudo tee /etc/systemd/system/industrial-collector.service > /dev/null << EOF
[Unit]
Description=Industrial Data Collector (Modbus/IEC104 -> MQTT + Web UI)
After=network.target

[Service]
ExecStart=/usr/local/bin/industrial_collector /etc/industrial_collector/config.json
WorkingDirectory=/etc/industrial_collector
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable industrial-collector
sudo systemctl start  industrial-collector
echo "服务已启动，查看日志: journalctl -fu industrial-collector"
