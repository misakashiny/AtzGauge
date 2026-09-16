# serial-watch.py — 复位板子并抓取串口日志（掉线自动重连版）
#
# 用法：python tools/serial-watch.py [COM3] [秒数]
#
# 说明：
#   - ESP32-S3 原生 USB 口波特率无意义，填 115200 即可。
#   - 板子软复位 / 重新上电时 USB 会重新枚举，串口句柄会失效；
#     这里捕获异常后自动重开，不会整个脚本退出。

import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

deadline = time.time() + seconds
total = 0
dev = None


def open_port():
    s = serial.Serial(port, 115200, timeout=0.2)
    # 复位脉冲（RTS -> EN）
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.15)
    s.setRTS(False)
    time.sleep(0.15)
    s.reset_input_buffer()
    return s


while time.time() < deadline:
    if dev is None:
        try:
            dev = open_port()
        except Exception as e:  # 端口还没枚举出来，等一下再试
            print(f"\n--- [watch] 打开 {port} 失败，重试中：{e} ---", flush=True)
            time.sleep(1.0)
            continue

    try:
        chunk = dev.read(4096)
        if chunk:
            total += len(chunk)
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    except Exception as e:
        print(f"\n--- [watch] 串口中断（{e}），尝试重连 ---", flush=True)
        try:
            dev.close()
        except Exception:
            pass
        dev = None
        time.sleep(1.0)

if dev is not None:
    try:
        dev.close()
    except Exception:
        pass

print(f"\n--- 共收到 {total} 字节 ---")
