#!/usr/bin/env python3
"""보드가 USB 시리얼로 보내는 내용을 그대로 터미널에 출력.

보드를 뽑았다 꽂거나 리셋해도 자동으로 다시 연결해서 계속 읽는다.
사용법: python3 tools/serial_cat.py [/dev/cu.usbmodemXXXX]
포트를 생략하면 첫 번째 usbmodem 포트를 사용. 종료는 Ctrl+C.
"""
import glob
import os
import subprocess
import sys
import time


def find_port(preferred):
    # 포트를 지정했으면 그 포트만 쓴다. (사라졌을 때 다른 보드 포트로 옮겨 붙으면
    # 그 보드 업로드를 막아 버린 사고가 있었음)
    if preferred:
        return preferred if os.path.exists(preferred) else None
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports[0] if ports else None


def read_until_disconnect(port):
    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY)
    subprocess.run(["stty", "-f", port, "115200"], check=False)
    print(f"--- {port} 연결됨 ---", flush=True)
    buf = b""
    try:
        while True:
            chunk = os.read(fd, 256)
            if not chunk:
                time.sleep(0.05)
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode(errors="ignore").strip()
                if text:
                    print(f"{time.strftime('%H:%M:%S')}  {text}", flush=True)
    finally:
        os.close(fd)


def main():
    preferred = sys.argv[1] if len(sys.argv) > 1 else None
    print("보드 출력 읽는 중... (종료: Ctrl+C)\n", flush=True)
    last_status = None

    def status(msg):
        # 같은 상태 메시지를 반복해서 찍지 않음
        nonlocal last_status
        if msg != last_status:
            print(f"--- {msg} ---", flush=True)
            last_status = msg

    try:
        while True:
            port = find_port(preferred)
            if not port:
                status("보드 연결 기다리는 중")
                time.sleep(0.5)
                continue
            try:
                last_status = None
                read_until_disconnect(port)
            except OSError as e:
                if e.errno == 16:
                    status("다른 프로그램(Arduino IDE 시리얼 모니터 등)이 포트를 사용 중")
                else:
                    status("보드 연결 끊김")
                time.sleep(0.5)
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()
