#include "indicator.h"
#include "config.h"
#include "battery.h"
#include "settings.h"

// ============================================================================
//  心跳指示灯的时序
//
//      |<---------- INDICATOR_PERIOD_MS (10 s) ---------->|
//      |####|............................................|####|..........
//      ↑ 亮 s_durationDs × 0.1 s（默认 1.0 s）
//
//  - 默认关闭：上电、复位都是灭的，必须由上位机打开（安全默认）。
//  - 低电量时用另一种颜色（红），一眼能看出该充电了。
//  - 只在“亮/灭”翻转的那一次真正写 LED，避免每 10 ms 刷 WS2812 占用 RMT。
// ============================================================================

static bool     s_enabled    = false;                            // 心跳开关，默认关
static uint16_t s_durationDs = INDICATOR_DEFAULT_DURATION_DS;    // 亮灯时长，0.1 s 单位
static bool     s_ledOn      = false;                            // LED 当前物理状态
static uint32_t s_cycleMs    = 0;                                // 当前心跳周期起点
static uint32_t s_flashUntil = 0;                                // 手动闪灯的截止时刻

static void writeLed(bool on)
{
    if (on == s_ledOn) {
        return;
    }
    s_ledOn = on;

    uint8_t r = 0, g = 0, b = 0;
    if (on) {
        if (isBatteryCritical()) {
            r = INDICATOR_LOW_COLOR_R; g = INDICATOR_LOW_COLOR_G; b = INDICATOR_LOW_COLOR_B;
        } else {
            r = INDICATOR_COLOR_R; g = INDICATOR_COLOR_G; b = INDICATOR_COLOR_B;
        }
    }

#if INDICATOR_NEOPIXEL
    neopixelWrite(INDICATOR_PIN, r, g, b);   // Arduino core 自带，不需要额外库
#else
    digitalWrite(INDICATOR_PIN, on ? HIGH : LOW);
#endif
}

// 只把灯物理熄灭，不动 s_enabled / 不写 NVS（关机时用，不能把用户的设置冲掉）
static void ledPhysicalOff()
{
    s_ledOn = true;          // 先假定亮着，好让下面的 writeLed(false) 真的写一帧全黑
    writeLed(false);
}

void initIndicator()
{
#if !INDICATOR_NEOPIXEL
    // 普通 LED 才需要配引脚；WS2812 由 neopixelWrite 自己管。
    pinMode(INDICATOR_PIN, OUTPUT);
    digitalWrite(INDICATOR_PIN, LOW);
#endif

    // 从 NVS 读回上次的设置（按 RST / 断电重上电都还在）；读不到就是「默认关闭」
    s_durationDs = settingsGetU16(SET_KEY_LED_DS, INDICATOR_DEFAULT_DURATION_DS);
    s_enabled    = settingsGetU8(SET_KEY_LED_EN, 0) != 0;
    s_cycleMs    = millis();          // 从开机这一刻起算周期，避免刚上电就闪

    ledPhysicalOff();                 // 复位后 WS2812 可能还留着上次的颜色

    LOGF("[indicator] heartbeat %s, %u ms on / %u ms period (from NVS)\n",
         s_enabled ? "ON" : "OFF", (unsigned)s_durationDs * 100, INDICATOR_PERIOD_MS);
}

void indicatorOff()
{
    // 关机专用：灭灯 + 停心跳，但**不写 NVS**，下次开机还是用户设置的开关状态。
    s_enabled    = false;
    s_flashUntil = 0;
    ledPhysicalOff();
}

void setIndicatorConfig(bool enable, uint16_t durationDs)
{
    if (durationDs > 0) {
        s_durationDs = (durationDs > INDICATOR_MAX_DURATION_DS) ? INDICATOR_MAX_DURATION_DS : durationDs;
        settingsPutU16(SET_KEY_LED_DS, s_durationDs);      // 存起来，RST/断电后还在
    }
    if (s_enabled != enable) {
        s_enabled = enable;
        s_cycleMs = millis();      // 打开/关闭时重新起一个周期，避免刚开就立刻闪
        settingsPutU8(SET_KEY_LED_EN, enable ? 1 : 0);
    }
}

void getIndicatorConfig(bool* enable, uint16_t* durationDs)
{
    if (enable)     *enable     = s_enabled;
    if (durationDs) *durationDs = s_durationDs;
}

void indicatorFlash(uint16_t durationDs)
{
    uint16_t ds = durationDs ? durationDs : s_durationDs;
    if (ds > INDICATOR_MAX_DURATION_DS) {
        ds = INDICATOR_MAX_DURATION_DS;
    }
    s_flashUntil = millis() + (uint32_t)ds * 100;
}

void updateIndicator()
{
    uint32_t now = millis();
    bool on = false;

    if ((int32_t)(now - s_flashUntil) < 0) {
        on = true;                          // 手动闪灯优先
    } else if (s_enabled) {
        if ((uint32_t)(now - s_cycleMs) >= INDICATOR_PERIOD_MS) {
            s_cycleMs = now;                // 进入下一个 10 s 周期
        }
        on = ((uint32_t)(now - s_cycleMs) < (uint32_t)s_durationDs * 100);
    }

    writeLed(on);
}
