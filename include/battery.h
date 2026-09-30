#pragma once

#include <Arduino.h>

// 初始化 ADC（衰减、分辨率）。
void initBattery();

// 立即做一次多次采样，返回电池电压（毫伏）。已做去极值 + 均值。
uint16_t readBatteryMillivolts();

// 电压 -> 估算电量百分比（查表 + 线性插值），返回 0~100。
uint8_t voltageToPercent(uint16_t millivolts);

// 按 BATTERY_UPDATE_INTERVAL_MS 周期刷新内部缓存；BLE 查询直接返回缓存，不阻塞。
// 返回 true 表示本次调用真的更新了数据。
bool updateBattery();

uint16_t getBatteryMillivolts();    // 最近一次缓存电压
uint8_t  getBatteryPercent();       // 最近一次缓存电量

// 读数是否可信：>= BATTERY_MIN_PLAUSIBLE_MV。不可信时下面两个判断都返回 false。
bool batteryReadingPlausible();

bool isBatteryLow();                // < BATTERY_WARN_MV
bool isBatteryCritical();           // < BATTERY_CRITICAL_MV
