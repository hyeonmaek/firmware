#!/bin/bash
# NU-87 TinyDK에 버튼 없이 빌드+업로드 (Auto Flash 모드). 포트 이름은 USB 꽂는 위치마다 바뀌므로
# USB-시리얼 칩(CP2102N) 일련번호로 찾는다.
# 사용법: tools/upload_nu87.sh [스케치 폴더(기본 NU87_Gateway)]
set -e
SKETCH="${1:-NU87_Gateway}"
FQBN="realtek:AmebaD:Ameba_AMB26"
CP2102_SN="fe7cb9455939f111b2c7ab6f984367d3"
cd "$(dirname "$0")/.."

PORT=$(arduino-cli board list --format json | python3 -c "
import json,sys
for p in json.load(sys.stdin)['detected_ports']:
    if p['port'].get('properties',{}).get('serialNumber')=='$CP2102_SN':
        print(p['port']['address']); break")
if [ -z "$PORT" ]; then echo "NU-87이 USB에 없음"; exit 1; fi
pkill -f "serial_cat.py $PORT" 2>/dev/null || true

arduino-cli compile --fqbn "$FQBN" "$SKETCH" 2>&1 | grep -E "error|Sketch uses"
arduino-cli upload --fqbn "$FQBN:AutoUploadMode=Enable" -p "$PORT" "$SKETCH" 2>&1 | tr '\r' '\n' | grep -E "error|successfully" | head -3
echo "PORT=$PORT"
