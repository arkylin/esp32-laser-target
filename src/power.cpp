#include "power.h"
#include "config.h"
#include "laser.h"
#include "indicator.h"
#include "battery.h"
#include "ble_server.h"
#include "settings.h"

#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <driver/gpio.h>

// ============================================================================
//  关机 / 开机的状态记忆
//
//  RST 键 = EN 引脚，按下去只会「复位」，固件必须自己区分开机还是关机。做法：
//  在 RTC 域里放一个 32 位标记（掉电才丢，复位/深睡都不丢）：
//
//      MAGIC_ON  = 上次是开机状态   MAGIC_OFF = 上次是关机状态   其它 = 不知道（冷启动）
//
//  开机流程：
//      上电复位(POWERON)            -> 一律开机（兜底：绝不停在关机态）
//      RTC==MAGIC_OFF（上次关机）    -> 开机
//      RTC==MAGIC_ON + 外部引脚复位   -> 用户按 RST ⇒ 关机
//      其它（崩溃/看门狗/烧录复位）   -> 开机（fail-safe，异常复位不变成关机）
//
//  这个标记即使因为掉电被清成垃圾值也没关系：认不出就按「开机」处理，
//  最坏结果是「按 RST 不关机」，不会出现「按 RST 开不起来」。
// ============================================================================

#define PWR_MAGIC       0x4C415345UL                  // "LASE"
#define PWR_MAGIC_ON    (PWR_MAGIC)                   // 上次开机
#define PWR_MAGIC_OFF   (PWR_MAGIC ^ 0xFFFFFFFFUL)    // 上次已关机

// RTC_NOINIT：启动代码不会初始化它，复位/深睡都保留，只有彻底掉电才变垃圾值。
RTC_NOINIT_ATTR static uint32_t s_rtcPower;

static const char* resetReasonName(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON 上电";
        case ESP_RST_EXT:       return "EXT 外部引脚(RST)";
        case ESP_RST_SW:        return "SW 软件复位";
        case ESP_RST_PANIC:     return "PANIC 崩溃";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP 深睡唤醒";
        case ESP_RST_BROWNOUT:  return "BROWNOUT 欠压";
        // 注：IDF 4.4（Arduino core 2.0.17）的枚举里还没有 ESP_RST_USB / ESP_RST_JTAG，
        // 这两种（烧录/串口复位）会落到 default，行为一样是「照常开机」。
        default:                return "其它（含串口/JTAG 复位）";
    }
}

static void setState(uint32_t v)
{
    s_rtcPower = v;
}

void powerOff()
{
    LOGLN("[power] === shutdown ===");

    // 1) 激光先关：安全第一
    laserOff();

#if POWER_HOLD_LASER_PIN
    // 深睡时数字域掉电，GPIO 会变成高阻；如果激光模块的 S 脚悬空被拉高就会一直亮，
    // 所以把 LOW 电平「保持」住（gpio_hold_en + 深睡保持开关）。
    gpio_hold_en((gpio_num_t)LASER_PIN);
    gpio_deep_sleep_hold_en();
#endif

    // 2) 指示灯熄灭：WS2812 自己会记住最后一个颜色，不写黑的话它会一直亮着
    indicatorOff();

    // 3) 记住「已关机」，这样下次按 RST 会被认成开机
    setState(PWR_MAGIC_OFF);

#if POWER_BOOT_KEY_WAKE
    // 可选项（默认关）：深睡时按住 BOOT 键当开机键。
    // 注意 GPIO0 是启动模式 strap 脚，唤醒瞬间它还是低电平，芯片会落进
    // 串口下载模式而不是正常开机 —— 所以默认必须关着，开机用 RST / 断电。
    esp_sleep_enable_ext0_wakeup((gpio_num_t)POWER_BUTTON_PIN, 0);
#endif

    // 4) 这里刻意不调 NimBLEDevice::deinit()：深睡会把数字域断电，栈会一起停；
    //    在 BLE 上下文里折腾栈反而有死锁风险。
    LOGLN("[power] deep sleep now - press RST (or power cycle) to power on");
    Serial.flush();
    delay(60);                 // 等 WS2812 把最后一帧锁存完、串口把日志发完

    esp_deep_sleep_start();    // 没配任何唤醒源 ⇒ 只能按 RST / 断电开机
    // 不会走到这里
}

void initPower()
{
    esp_reset_reason_t reason = esp_reset_reason();
    uint32_t state = s_rtcPower;

    // 空闲自动关机的运行时设置（settingsBegin 已在 main setup 里先跑过）
    loadAutoSleepConfig();

    LOGF("[power] reset reason = %d (%s), rtc state = %s\n",
         (int)reason, resetReasonName(reason),
         state == PWR_MAGIC_ON ? "ON" : (state == PWR_MAGIC_OFF ? "OFF" : "unknown"));

    pinMode(POWER_BUTTON_PIN, INPUT_PULLUP);

    // ---- 1) 上次是关机状态 ⇒ 这次的复位就是用户按 RST 开机 ----
    if (state == PWR_MAGIC_OFF) {
        setState(PWR_MAGIC_ON);
        LOGLN("[power] previous state OFF -> power ON");
        return;
    }

    // ---- 2) 冷启动 / 状态丢失 ⇒ 一律开机（绝不停在关机态）----
    if (state != PWR_MAGIC_ON) {
        setState(PWR_MAGIC_ON);
        LOGLN("[power] cold boot -> power ON");
        return;
    }

    // ---- 3) 上次就在运行，而这次复位了：只有 RST 才算「关机」----
    setState(PWR_MAGIC_ON);    // 先把状态摆正，后面无论走哪条路都成立

    if (reason == ESP_RST_POWERON) {
#if POWER_RST_ACCEPT_POWERON
        LOGLN("[power] POWERON reset treated as RST key -> shutdown");
        powerOff();
#else
        LOGLN("[power] POWERON reset -> power ON (POWER_RST_ACCEPT_POWERON=0)");
#endif
        return;
    }

#if POWER_RST_AS_SWITCH
    if (reason == ESP_RST_EXT) {
        LOGLN("[power] RST pressed while running -> shutdown");
        powerOff();
        return;
    }
#endif

    // 崩溃、看门狗、烧录/串口复位……都照常启动，绝不因为异常复位变成关机
    LOGF("[power] reset reason %d is not the RST key -> keep running\n", (int)reason);
}

// ---------------------------------------------------- 空闲自动关机 ----
// 运行时配置（BLE 特征 7d000008 / 网页可改，NVS 持久化，键 autoEn/autoMin）。
// 插着 USB（GPIO3 实测 > AUTO_SLEEP_USB_MV）时永不触发：开发调试、充电、台架演示不打扰。
static bool     s_autoEnable  = AUTO_SLEEP_DEFAULT_ENABLE;
static uint16_t s_autoMinutes = AUTO_SLEEP_DEFAULT_MINUTES;
static uint32_t s_idleSinceMs = 0;              // 静默计时起点，任何「值得保持开机」的事件都清零

static uint16_t clampMinutes(uint16_t minutes)
{
    if (minutes < AUTO_SLEEP_MIN_MINUTES) return AUTO_SLEEP_MIN_MINUTES;
    if (minutes > AUTO_SLEEP_MAX_MINUTES) return AUTO_SLEEP_MAX_MINUTES;
    return minutes;
}

void loadAutoSleepConfig()
{
    s_autoEnable  = settingsGetU8(SET_KEY_AUTO_EN, AUTO_SLEEP_DEFAULT_ENABLE ? 1 : 0) != 0;
    s_autoMinutes = clampMinutes(settingsGetU16(SET_KEY_AUTO_MIN, AUTO_SLEEP_DEFAULT_MINUTES));
    s_idleSinceMs = millis();
    LOGF("[power] auto sleep %s, idle timeout %u min (from NVS)\n",
         s_autoEnable ? "ON" : "OFF", (unsigned)s_autoMinutes);
}

void getAutoSleepConfig(bool* enable, uint16_t* minutes)
{
    if (enable)  *enable  = s_autoEnable;
    if (minutes) *minutes = s_autoMinutes;
}

void setAutoSleepConfig(bool enable, uint16_t minutes)
{
    s_autoMinutes = clampMinutes(minutes);
    settingsPutU16(SET_KEY_AUTO_MIN, s_autoMinutes);
    if (s_autoEnable != enable) {
        s_autoEnable = enable;
        settingsPutU8(SET_KEY_AUTO_EN, enable ? 1 : 0);
    }
    s_idleSinceMs = millis();      // 改配置视为一次活动，从头计时
    LOGF("[power] auto sleep set: %s, %u min (saved)\n",
         s_autoEnable ? "ON" : "OFF", (unsigned)s_autoMinutes);
}

void updateAutoSleep()
{
    if (!s_autoEnable) {
        return;
    }

    // 有人连着就永不超时（给对靶程序留着随时开激光的通道）。
    if (isBLEConnected()) {
        s_idleSinceMs = millis();
        return;
    }

    // 插着 USB（GPIO3 读数远高于电池上限）不自动关机。
    // 读数每 5 s 刷新一次（updateBattery 节流），对分钟级的超时来说足够快。
    if (batteryReadingPlausible() && getBatteryMillivolts() >= AUTO_SLEEP_USB_MV) {
        s_idleSinceMs = millis();
        return;
    }

    uint32_t timeoutMs = (uint32_t)s_autoMinutes * 60000UL;
    if ((uint32_t)(millis() - s_idleSinceMs) < timeoutMs) {
        return;
    }

    LOGF("[power] no BLE client for %u min on battery -> auto shutdown\n",
         (unsigned)s_autoMinutes);
    powerOff();
}

void updatePower()
{
    static bool     wasDown = false;
    static uint32_t downMs  = 0;

    uint32_t now = millis();

    // 开机后一小段时间内不认这个键：避免刚上电（或手还按着）就被关掉。
    if (now < POWER_BUTTON_IGNORE_MS) {
        return;
    }

    bool down = (digitalRead(POWER_BUTTON_PIN) == LOW);
    if (down && !wasDown) {
        downMs = now;
    }
    wasDown = down;

    if (down && (uint32_t)(now - downMs) >= POWER_BUTTON_DEBOUNCE_MS) {
        LOGLN("[power] BOOT button pressed -> shutdown");
        powerOff();
    }
}
