#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESP32-S3 激光对靶模块 BLE 控制客户端（基于 bleak）。

用法:
    python laser_client.py on                 # 开激光
    python laser_client.py off                # 关激光
    python laser_client.py status             # 查询状态 + 电量
    python laser_client.py monitor            # 订阅通知，持续打印（Ctrl+C 退出）
    python laser_client.py stress --count 100 # 单次连接内连续开关 100 轮并核对状态
    python laser_client.py on --address AA:BB:CC:DD:EE:FF
    python laser_client.py status --name LaserTarget-S3 --timeout 10

输出示例:
    Laser: ON
    Battery: 83 %
    Voltage: 3.921 V

依赖:
    pip install bleak
"""

import argparse
import asyncio
import sys
from datetime import datetime

from bleak import BleakClient, BleakScanner

# ----------------------------------------------------------------- 常量 ----
DEFAULT_NAME = "LaserTarget-S3"

SERVICE_UUID  = "7d000001-7a5b-4a86-8d3f-123456789abc"
CHR_CONTROL   = "7d000002-7a5b-4a86-8d3f-123456789abc"  # WRITE      : 0x00 OFF / 0x01 ON
CHR_STATUS    = "7d000003-7a5b-4a86-8d3f-123456789abc"  # READ+NOTIFY: 0/1
CHR_SOC       = "7d000004-7a5b-4a86-8d3f-123456789abc"  # READ+NOTIFY: 0~100
CHR_VOLTAGE   = "7d000005-7a5b-4a86-8d3f-123456789abc"  # READ+NOTIFY: uint16 mV (little-endian)

CMD_OFF = b"\x00"
CMD_ON  = b"\x01"


# --------------------------------------------------------------- 工具函数 ----
async def resolve_target(address: str | None, name: str, timeout: float) -> str:
    """返回要连接的地址。给了 --address 就直接用，否则按名字扫描。"""
    if address:
        return address

    print(f"[scan] 正在查找 \"{name}\" ...", flush=True)
    device = await BleakScanner.find_device_by_name(name, timeout=timeout)
    if device is None:
        raise SystemExit(
            f"找不到设备 \"{name}\"。\n"
            f"  1) 确认模块已上电、固件已烧录、未被别的程序占用；\n"
            f"  2) 先跑 python scan_laser.py 看看能不能扫到；\n"
            f"  3) 或用 --address <MAC/UUID> 直接指定。"
        )
    print(f"[scan] 找到 {device.address}", flush=True)
    return device.address


async def read_laser_state(client: BleakClient) -> bool | None:
    raw = await client.read_gatt_char(CHR_STATUS)
    return bool(raw[0]) if raw else None


async def read_battery(client: BleakClient):
    soc_raw = await client.read_gatt_char(CHR_SOC)
    mv_raw = await client.read_gatt_char(CHR_VOLTAGE)
    soc = soc_raw[0] if soc_raw else 0
    mv = int.from_bytes(mv_raw[:2], byteorder="little", signed=False) if len(mv_raw) >= 2 else 0
    return soc, mv


def print_report(state: bool | None, soc: int, mv: int) -> None:
    state_txt = "ON" if state else "OFF" if state is not None else "?"
    print(f"Laser: {state_txt}")
    print(f"Battery: {soc} %")
    print(f"Voltage: {mv / 1000:.3f} V")


# ------------------------------------------------------------------ 命令 ----
async def cmd_set(client: BleakClient, turn_on: bool) -> None:
    payload = CMD_ON if turn_on else CMD_OFF
    await client.write_gatt_char(CHR_CONTROL, payload, response=True)
    print(f"[tx] 0x{payload[0]:02X} -> 激光应 {'开启' if turn_on else '关闭'}")

    # 状态由固件在状态变化时立刻 Notify，这里直接回读一次确认。
    await asyncio.sleep(0.15)
    state = await read_laser_state(client)
    soc, mv = await read_battery(client)
    print_report(state, soc, mv)

    expect = turn_on
    if state is not None and state != expect:
        print("!! 状态与指令不一致：检查是否处于低电量保护（< 3.40 V 时固件拒绝开激光）。")
        return
    print("[ok] 状态与指令一致")


async def cmd_status(client: BleakClient) -> None:
    state = await read_laser_state(client)
    soc, mv = await read_battery(client)
    print_report(state, soc, mv)


async def cmd_monitor(client: BleakClient, seconds: float | None) -> None:
    def make_handler(tag: str):
        def handler(_char, data: bytearray) -> None:
            ts = datetime.now().strftime("%H:%M:%S")
            if tag == "status":
                print(f"[{ts}] notify {tag:<7} = {'ON' if data[0] else 'OFF'}")
            elif tag == "soc":
                print(f"[{ts}] notify {tag:<7} = {data[0]} %")
            else:
                mv = int.from_bytes(bytes(data[:2]), "little")
                print(f"[{ts}] notify {tag:<7} = {mv} mV ({mv / 1000:.3f} V)")
        return handler

    await client.start_notify(CHR_STATUS, make_handler("status"))
    await client.start_notify(CHR_SOC, make_handler("soc"))
    await client.start_notify(CHR_VOLTAGE, make_handler("voltage"))

    state = await read_laser_state(client)
    soc, mv = await read_battery(client)
    print("--- 当前状态 ---")
    print_report(state, soc, mv)
    print("--- 等待通知 (Ctrl+C 退出) ---")

    try:
        if seconds:
            await asyncio.sleep(seconds)
        else:
            while client.is_connected:
                await asyncio.sleep(1.0)
    except (asyncio.CancelledError, KeyboardInterrupt):
        pass
    finally:
        for uuid in (CHR_STATUS, CHR_SOC, CHR_VOLTAGE):
            try:
                await client.stop_notify(uuid)
            except Exception:
                pass


async def cmd_stress(client: BleakClient, count: int, interval: float) -> None:
    """单次连接内连续开关 count 轮，每轮回读状态核对（Phase 1 稳定性测试）。"""
    print(f"开始连续开关 {count} 轮（共 {count * 2} 次动作），每次间隔 {interval * 1000:.0f} ms ...")
    ok = 0
    total = 0
    for i in range(1, count + 1):
        for want_on in (True, False):
            await client.write_gatt_char(CHR_CONTROL, CMD_ON if want_on else CMD_OFF, response=True)
            await asyncio.sleep(interval)
            total += 1
            state = await read_laser_state(client)
            if state == want_on:
                ok += 1
            else:
                print(f"  !! 第 {i} 轮 {'ON' if want_on else 'OFF'}: 回读 = {state}")
    print(f"完成：状态一致 {ok}/{total}")
    if ok != total:
        print("!! 存在不一致，检查接线（激光 S 是否接 GPIO4）、供电与低电量保护。")
        return
    print("[ok] 连续开关稳定")


# -------------------------------------------------------------------- 主流程 ----
async def run(args) -> int:
    target = await resolve_target(args.address, args.name, args.timeout)

    print(f"[conn] 连接 {target} ...", flush=True)
    async with BleakClient(target, timeout=args.timeout) as client:
        if not client.is_connected:
            print("连接失败。")
            return 1
        print("[conn] 已连接")

        if args.command == "on":
            await cmd_set(client, True)
        elif args.command == "off":
            await cmd_set(client, False)
        elif args.command == "status":
            await cmd_status(client)
        elif args.command == "monitor":
            await cmd_monitor(client, args.seconds)
        elif args.command == "stress":
            await cmd_stress(client, args.count, args.interval)
        else:
            print(f"未知命令: {args.command}")
            return 2
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="ESP32-S3 激光对靶模块 BLE 控制客户端",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("command", choices=["on", "off", "status", "monitor", "stress"])
    parser.add_argument("--name", default=DEFAULT_NAME, help=f"BLE 设备名，默认 {DEFAULT_NAME}")
    parser.add_argument("--address", default=None, help="直接指定 MAC/UUID，跳过扫描")
    parser.add_argument("--timeout", type=float, default=10.0, help="扫描/连接超时(秒)，默认 10")
    parser.add_argument("--seconds", type=float, default=None, help="monitor 模式的持续时间(秒)")
    parser.add_argument("--count", type=int, default=100, help="stress 模式的轮数，默认 100")
    parser.add_argument("--interval", type=float, default=0.05, help="stress 模式每次动作后的等待(秒)，默认 0.05")
    args = parser.parse_args()

    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\n已中断。")
        return 130


if __name__ == "__main__":
    sys.exit(main())
