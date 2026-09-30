#pragma once

#include <Arduino.h>

// 设置持久化（NVS / Preferences）
//
// 用途：网页上改的「心跳灯开关/时长」「广播间隔」要**记住** —— 关机、按 RST、
//       断电重上电、甚至重新烧固件（不 erase_flash）之后都还在。
// 失败兜底：NVS 打不开（分区表没有 nvs 等）时全部退化成默认值且不保存，绝不阻塞启动。

void settingsBegin();

uint8_t  settingsGetU8(const char* key, uint8_t def);
uint16_t settingsGetU16(const char* key, uint16_t def);
void     settingsPutU8(const char* key, uint8_t v);
void     settingsPutU16(const char* key, uint16_t v);

// 键名集中在这里，避免各个模块各写一份字符串写错。
#define SET_KEY_LED_EN      "ledEn"     // uint8  0/1   心跳灯开关（默认 0 = 关）
#define SET_KEY_LED_DS      "ledDs"     // uint16 0.1 s 心跳灯每次亮多久
#define SET_KEY_ADV_MS      "advMs"     // uint16 ms    广播间隔
#define SET_KEY_AUTO_EN     "autoEn"    // uint8  0/1   空闲自动关机开关（默认 1 = 开）
#define SET_KEY_AUTO_MIN    "autoMin"   // uint16 min   无人连接多久自动关机（默认 5）
