#!/bin/bash
# NU-40 DK에 버튼 없이 업로드: 1200bps 터치(1200bps 설정 후 DTR 내림) → 부트로더 → arduino-cli upload
# 사용법: tools/upload_nu40.sh <USB 일련번호> [스케치 폴더(기본 Tap_Talk)]
#   예: tools/upload_nu40.sh C19BFA17425596AD     (독수리-1, AD)
#       tools/upload_nu40.sh 2B579EEFB49117FA     (독수리-2, FA)
# 그 보드의 시리얼을 읽는 serial_cat.py는 자동으로 끈다. 빌드는 하지 않으므로 먼저 arduino-cli compile.
set -e
SN="$1"
SKETCH="${2:-Tap_Talk}"
FQBN="nucode:nrf52:nu40dk"
cd "$(dirname "$0")/.."

find_port() {
  arduino-cli board list --format json | python3 -c "
import json,sys
for p in json.load(sys.stdin)['detected_ports']:
    if p['port'].get('properties',{}).get('serialNumber')=='$SN':
        print(p['port']['address']); break"
}

PORT=$(find_port)
if [ -z "$PORT" ]; then echo "보드 $SN 이(가) USB에 없음"; exit 1; fi
pkill -f "serial_cat.py $PORT" 2>/dev/null || true

# 이미 부트로더 상태여도 터치는 무해하므로 항상 보낸다
python3 -c "
import os, sys, termios, fcntl, time
fd = os.open('$PORT', os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
a[4] = a[5] = termios.B1200
a[2] |= termios.HUPCL
termios.tcsetattr(fd, termios.TCSANOW, a)
TIOCSDTR, TIOCCDTR = 0x20007479, 0x20007478
fcntl.ioctl(fd, TIOCSDTR); time.sleep(0.2)
fcntl.ioctl(fd, TIOCCDTR); time.sleep(0.2)
os.close(fd)
"
sleep 4
PORT=$(find_port)
if [ -z "$PORT" ]; then echo "부트로더 포트를 못 찾음"; exit 1; fi

arduino-cli upload --fqbn "$FQBN" -p "$PORT" "$SKETCH" 2>&1 | grep -E "Device programmed|Failed|rror" | head -3
