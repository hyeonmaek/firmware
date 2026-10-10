#!/usr/bin/env python3
"""보드가 측정한 동료 신호 세기를 터미널에 막대그래프로 실시간 표시.

사용법: python3 tools/rssi_watch.py [/dev/cu.usbmodemXXXX]
포트를 생략하면 첫 번째 usbmodem 포트를 사용. 종료는 Ctrl+C.
"""
import glob
import os
import re
import subprocess
import sys

LINE = re.compile(r"raw=(-?\d+) avg=(-?\d+)")


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("보드 포트를 찾을 수 없습니다. USB 연결을 확인하세요.")
    return ports[0]


def bar(rssi):
    # -100(약함) ~ -40(강함) 범위를 0~30칸으로
    filled = max(0, min(30, int((rssi + 100) / 2)))
    return "█" * filled + "·" * (30 - filled)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    subprocess.run(["stty", "-f", port, "115200"], check=True)
    print(f"{port} 에서 읽는 중... (종료: Ctrl+C)\n")

    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY)
    buf = b""
    try:
        while True:
            chunk = os.read(fd, 256)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw_line, buf = buf.split(b"\n", 1)
                text = raw_line.decode(errors="ignore").strip()
                m = LINE.search(text)
                if m:
                    raw, avg = int(m.group(1)), int(m.group(2))
                    print(f"\r신호 {avg:4d} dBm  {bar(avg)}  (순간값 {raw:4d})   ", end="", flush=True)
                elif text.startswith("ERR"):
                    print(f"\r오류: {text:<60}", end="", flush=True)
                elif text == "LOST":
                    print(f"\r신호  --- dBm  {'두절 (동료 못 찾음)':<30}                   ", end="", flush=True)
    except KeyboardInterrupt:
        print()
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
