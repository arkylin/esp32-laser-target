#pragma once

#include <Arduino.h>

// 初始化 BLE GATT Server 并开始广播（设备名 DEVICE_NAME）。
void initBLE();

bool isBLEConnected();

// 在主循环里周期调用：
//   1) 执行上位机下发的待处理控制指令（含低电量拦截）；
//   2) 激光状态变化时 Notify Status；
//   3) 电池电量变化 >=1% 或距上次 >= 30 s 时 Notify SOC + Voltage。
void updateBLE();
