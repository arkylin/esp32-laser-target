// ============================================================================
//  ESP32-S3 SuperMini 无线激光对靶模块 — 主程序
//  方案文档: ESP32S3_SuperMini_Laser_BLE_Project_Plan.md
//
//  上电顺序刻意安排为：激光 OFF -> 电池 -> BLE，保证任何时刻都是安全状态。
//  Arduino ESP32 底层已跑在 FreeRTOS 上，第一版不额外创建任务。
// ============================================================================

#include <Arduino.h>

#include "config.h"
#include "laser.h"
#include "battery.h"
#include "indicator.h"
#include "power.h"
#include "settings.h"
#include "ble_server.h"

static uint32_t s_lastLowWarnMs = 0;
static bool     s_warnedImplausible = false;

void setup()
{
    // CPU 主频（省电旋钮，默认 240 最稳；改 160/80 前先确认串口日志还在）
    setCpuFrequencyMhz(CPU_FREQ_MHZ);

    Serial.begin(SERIAL_BAUD);
#if ARDUINO_USB_CDC_ON_BOOT
    // 等 USB CDC 枚举，否则最早的日志会丢。
    uint32_t t0 = millis();
    while (!Serial && (uint32_t)(millis() - t0) < SERIAL_WAIT_MS) {
        delay(10);
    }
#endif

    LOGLN();
    LOGLN("=== LaserTarget-S3 boot ===");
    LOGF("[cpu] %u MHz\n", (unsigned)getCpuFrequencyMhz());

    // 0) 设置存储：心跳灯/广播间隔都存在这里，必须在读设置之前打开
    settingsBegin();

    // 1) 激光：第一时间置于 OFF
    initLaser();
    LOGF("[laser] GPIO%d = OFF (safe default)\n", LASER_PIN);

    // 2) 开关机判定：运行中按了 RST 的话，这里就直接睡下去，后面的初始化都不做
    initPower();

    // 3) 电池：立刻读一次，供低电量保护与 BLE 初值使用
    initBattery();
    updateBattery();
    LOGF("[battery] %.3f V, %u %%\n",
         getBatteryMillivolts() / 1000.0f, getBatteryPercent());

    // 4) 心跳指示灯：默认关闭（电池供电时靠它看模块还在不在，由上位机打开）
    initIndicator();
    LOGF("[indicator] GPIO%d heartbeat OFF (default, %u ms period)\n",
         INDICATOR_PIN, INDICATOR_PERIOD_MS);

    // 5) BLE
    initBLE();

    LOGLN("=== ready ===");
}

void loop()
{
    // 关机键（板载 BOOT）：运行中按一下 → 关激光、灭灯、进 deep sleep
    updatePower();

    // 空闲自动关机：电池供电 + 无人连接超过 5 min → deep sleep（插着 USB 永不触发）
    updateAutoSleep();

    // 电池：每 5 s 刷新一次（函数内部自带节流）
    bool updated = updateBattery();

    // 分压电阻/电池还没接时，ADC 悬空会读到很低的值；提示一次，避免误判成 BLE 或激光坏了。
    if (updated && !batteryReadingPlausible() && !s_warnedImplausible) {
        s_warnedImplausible = true;
        LOGF("[battery] %u mV 不可信：分压/电池未接，低电量保护暂不生效（仍可正常开关激光）\n",
             getBatteryMillivolts());
    }

    // 低电量保护：临界电压强制关激光
    if (isBatteryCritical() && getLaserState()) {
        laserOff();
        LOGF("[safety] battery %u mV critical -> laser OFF\n",
             getBatteryMillivolts());
    }

    // 低电量警告：最多每 60 s 提示一次
    if (isBatteryLow()) {
        uint32_t now = millis();
        if ((uint32_t)(now - s_lastLowWarnMs) >= 60000) {
            s_lastLowWarnMs = now;
            LOGF("[safety] low battery: %u mV, %u %%\n",
                 getBatteryMillivolts(), getBatteryPercent());
        }
    }

    // BLE：执行上位机指令 + 状态/电量 Notify
    updateBLE();

    // 心跳指示灯：按 10 s 周期点灯（默认关闭时这里什么都不做）
    updateIndicator();

    delay(LOOP_INTERVAL_MS);
}
