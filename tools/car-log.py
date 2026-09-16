#!/usr/bin/env python3
# car-log.py — 车上抓日志：自动找串口 + 带时间戳存文件 + 同时打印到屏幕
#
# 用法：
#   python tools/car-log.py              # 自动找串口，默认抓 3600 秒
#   python tools/car-log.py COM3 1800    # 指定串口和时长（秒）
#
# 特点：
#   - 自动识别 ESP32 的 USB 串口（跳过主板自带的 COM1）
#   - 原始日志存到 backup/car-log-YYYYmmdd-HHMMSS.txt（含 ANSI 颜色码）
#   - 屏幕只显示"去掉颜色码"的干净版本，方便肉眼看
#   - 板子重启/USB 重新枚举时自动重连，不会中断

import os
import re
import sys
import time

import serial
import serial.tools.list_ports

ANSI = re.compile(r"\x1b\[[0-9;]*m")
BACKUP_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "backup")


def pick_port():
    """找 ESP32 的 USB 串口：优先描述里带 USB/Serial/JTAG 的，跳过 COM1（主板自带的）"""
    ports = [p for p in serial.tools.list_ports.comports() if p.device.upper() != "COM1"]
    if not ports:
        return None
    for p in ports:
        desc = f"{p.description or ''} {p.manufacturer or ''} {p.hwid or ''}".lower()
        if "usb" in desc or "serial" in desc or "jtag" in desc or "303a" in desc:
            return p.device
    return ports[0].device


def main():
    port = None
    seconds = 3600.0

    if len(sys.argv) > 1 and sys.argv[1].upper().startswith("COM"):
        port = sys.argv[1]
        if len(sys.argv) > 2:
            seconds = float(sys.argv[2])
    elif len(sys.argv) > 1:
        seconds = float(sys.argv[1])

    if port is None:
        port = pick_port()
        if port is None:
            print("找不到可用串口。请确认板子已用 USB 线连上，然后重试。")
            print("已知串口：")
            for p in serial.tools.list_ports.comports():
                print(f"   {p.device}  {p.description}")
            sys.exit(1)
        print(f"自动识别到串口：{port}")

    os.makedirs(BACKUP_DIR, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = os.path.join(BACKUP_DIR, f"car-log-{stamp}.txt")

    print(f"串口     : {port}")
    print(f"原始日志 : {log_path}")
    print(f"时长     : {seconds:.0f} 秒（Ctrl+C 可提前结束）")
    print("-" * 60)

    deadline = time.time() + seconds
    dev = None
    raw = open(log_path, "wb")
    total = 0

    try:
        while time.time() < deadline:
            if dev is None:
                try:
                    dev = serial.Serial(port, 115200, timeout=0.2)
                    dev.reset_input_buffer()
                    print(f"\n--- [已连接 {port}] ---")
                except Exception as e:
                    time.sleep(1.0)
                    continue
            try:
                chunk = dev.read(4096)
                if chunk:
                    total += len(chunk)
                    raw.write(chunk)
                    raw.flush()
                    text = ANSI.sub("", chunk.decode("utf-8", "replace"))
                    sys.stdout.write(text)
                    sys.stdout.flush()
            except Exception as e:
                print(f"\n--- [串口中断：{e}，尝试重连] ---")
                try:
                    dev.close()
                except Exception:
                    pass
                dev = None
                time.sleep(1.0)
    except KeyboardInterrupt:
        print("\n--- 手动停止 ---")
    finally:
        raw.close()
        if dev is not None:
            try:
                dev.close()
            except Exception:
                pass

    print("-" * 60)
    print(f"共收到 {total} 字节，已保存到：{log_path}")


if __name__ == "__main__":
    main()
