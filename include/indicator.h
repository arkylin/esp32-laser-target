#pragma once

#include <Arduino.h>

// 心跳指示灯（板载 WS2812 / GPIO48）
//
// 用途：电池供电时看不出模块是不是还在工作（没有 USB 灯），
//       所以每 INDICATOR_PERIOD_MS（默认 10 s）亮一次，亮 INDICATOR_DURATION 那么久。
//       默认关闭，由上位机（网页 / nRF Connect / bleak）通过 BLE 打开。

// 初始化：默认关闭心跳，并把 LED 物理熄灭（复位后 WS2812 可能还保持上次的颜色）。
void initIndicator();

// 主循环调用：按心跳/手动闪灯的时序真正去点灯。
void updateIndicator();

// 设置心跳开关与亮灯时长。durationDs = 0 表示沿用当前值；单位 0.1 s，上限见 config.h。
void setIndicatorConfig(bool enable, uint16_t durationDs);

// 读回当前配置（供 BLE READ 特征缓存使用）。
void getIndicatorConfig(bool* enable, uint16_t* durationDs);

// 立刻按指定时长闪一次（durationDs = 0 用当前配置），不受开关状态影响。
// 用途：调灯时不用等满一个心跳周期就知道灯亮不亮。
void indicatorFlash(uint16_t durationDs);

// 物理熄灭并关掉心跳（关机/深睡前必须调用：WS2812 会自己记住最后一个颜色，
// 不写一帧全黑的话，模块断电前会一直亮着）。
void indicatorOff();
