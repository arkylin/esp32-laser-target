#pragma once

#include <Arduino.h>

// 关机 / 开机（电池模式下的省电开关）
//
//   关机 = 进 deep sleep：激光 OFF、指示灯灭、蓝牙停、CPU 停；按 RST 或断电才能再开机。
//   开机 = 按 RST（复位）或重新上电。
//
// 触发关机的三条路（任意一条都行）：
//   1) 板载 BOOT 键（GPIO0）运行中按一下 —— 最可靠，和复位无关；
//   2) 网页 / 上位机写 BLE Control 特征 0x02；
//   3) 运行中按 RST（靠 RTC 里的状态记忆 + 复位原因判断，见 config.h 说明）。

// 开机时最先调用：判断这次复位是「开机」还是「用户按 RST 关机」。
// 如果是关机，本函数不会返回（直接睡下去）。
void initPower();

// 主循环调用：轮询 BOOT 键。
void updatePower();

// 空闲自动关机（运行时可改，BLE 特征 7d000008 / 网页「省电设置」，存 NVS）。
// 条件 = 开关打开 + 没人连接连续 N 分钟 + 没插 USB（GPIO3 读数高 = USB 供电，永不触发）。
// initPower() 里会从 NVS 恢复上次的设置。
void loadAutoSleepConfig();
void getAutoSleepConfig(bool* enable, uint16_t* minutes);
void setAutoSleepConfig(bool enable, uint16_t minutes);   // 会写 NVS 并重置空闲计时

// 主循环调用：空闲自动关机。
void updateAutoSleep();

// 关机：关激光、灭灯、把激光脚钉住，然后 deep sleep。不会返回。
void powerOff();
