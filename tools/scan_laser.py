#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""扫描附近的 BLE 设备，重点标出激光对靶模块 LaserTarget-*。

用法:
    python scan_laser.py                # 扫描 6 秒
    python scan_laser.py -t 10          # 扫描 10 秒
    python scan_laser.py --all          # 打印所有设备（默认只打印有名字的）
    python scan_laser.py --filter Laser # 只打印名字包含该字串的设备

依赖:
    pip install bleak
"""

import argparse
import asyncio
import sys

from bleak import BleakScanner

DEFAULT_NAME_PREFIX = "LaserTarget"


async def scan(timeout: float):
    # return_adv=True 才能拿到 RSSI 等广播信息；老版本 bleak 没有这个参数。
    try:
        return await BleakScanner.discover(timeout=timeout, return_adv=True)
    except TypeError:
        return await BleakScanner.discover(timeout=timeout)


def main() -> int:
    parser = argparse.ArgumentParser(description="扫描 BLE 设备，标出激光模块")
    parser.add_argument("-t", "--timeout", type=float, default=6.0, help="扫描时长(秒)，默认 6")
    parser.add_argument("--all", action="store_true", help="打印没有名字的设备")
    parser.add_argument("--filter", default=None, help="只打印名字包含该字串的设备")
    args = parser.parse_args()

    print(f"正在扫描 {args.timeout:.0f} s ...", flush=True)
    found = asyncio.run(scan(args.timeout))

    # 兼容两种返回结构
    rows = []
    if isinstance(found, dict):                       # {address: (device, adv)}
        for addr, item in found.items():
            device, adv = item
            rows.append((device.name or "", addr, getattr(adv, "rssi", None)))
    else:                                             # [BLEDevice]
        for device in found:
            rows.append((device.name or "", device.address, getattr(device, "rssi", None)))

    rows.sort(key=lambda r: (r[2] is None, -(r[2] or 0)))

    hit = 0
    for name, addr, rssi in rows:
        matched = DEFAULT_NAME_PREFIX.lower() in name.lower()
        if args.filter and args.filter.lower() not in name.lower():
            continue
        if not name and not args.all and not args.filter:
            continue
        rssi_txt = f"{rssi:>4} dBm" if isinstance(rssi, int) else "   ? dBm"
        mark = "  <== LASER" if matched else ""
        print(f"  {addr:<20} {rssi_txt}  {name or '(无名)'}{mark}")
        if matched:
            hit += 1

    print(f"\n共 {len(rows)} 个设备，其中激光模块 {hit} 个。")
    if hit == 0:
        print("没找到 LaserTarget-*：确认模块已上电、未被其它主机占用、距离不要太远。")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
