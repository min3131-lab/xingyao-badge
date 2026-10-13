#!/usr/bin/env python3
"""抓取启动日志，不发送控制命令；原始日志仅保存在本机备份目录。"""
import argparse
import os
import re
import time
from pathlib import Path
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('port')
parser.add_argument('output', type=Path)
parser.add_argument('--seconds', type=int, default=35)
args = parser.parse_args()
port = serial.Serial(port=None, baudrate=115200, timeout=0.25)
port.dtr = False
port.rts = False
port.port = args.port
port.open()
args.output.parent.mkdir(parents=True, exist_ok=True)
with args.output.open('wb') as output:
    os.chmod(args.output, 0o600)
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        data = port.read(4096)
        if data:
            output.write(data)
            output.flush()
port.close()
log = args.output.read_bytes().decode('utf-8', errors='replace')
for line in log.splitlines():
    if re.search(r'AnimeBadge|BadgeMotion|Touch panel|CO5300|CST9217|PSRAM|PSRAM|AudioCodec|AudioService|Display|StateMachine|free |minimum free|Guru Meditation|abort\(|assert|ESP_ERROR_CHECK|Backtrace|boot: Loaded|main_task|徽章屏幕', line, re.I):
        print(re.sub(r'\x1b\[[0-9;]*m', '', line))
