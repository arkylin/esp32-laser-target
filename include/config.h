#pragma once

// ============================================================================
//  全部硬件引脚、阈值、UUID、时间参数集中在这里。
//  改硬件只改这个文件。
// ============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------- 设备 ----
#define DEVICE_NAME                 "LaserTarget-S3"
#define SERIAL_BAUD                 115200
#define SERIAL_WAIT_MS              1500    // 等 USB CDC 串口枚举的最长时间

// ---------------------------------------------------------------- 激光 ----
// GPIO4 -> 激光模块 S 脚；模块自带三极管驱动，不需要额外 MOSFET。
#define LASER_PIN                   4
#define LASER_ACTIVE_HIGH           1       // 1 = 高电平点亮
#if LASER_ACTIVE_HIGH
  #define LASER_ON_LEVEL            HIGH
  #define LASER_OFF_LEVEL           LOW
#else
  #define LASER_ON_LEVEL            LOW
  #define LASER_OFF_LEVEL           HIGH
#endif

// ------------------------------------------------------------ 电池 ADC ----
// 【当前方案 A】直接用板载 GPIO3 那条分压（零焊接）。
//   板载路径：BAT → 肖特基二极管 → 系统节点 → 100k/100k → GPIO3
//   ⇒ 量到的是“二极管之后”的系统节点：比电池低一个压降；
//     插 USB 时读到的是 USB 电压（≈4.9 V，电量会显示 100% = 充电中）。
//   两块板子的实测数据与判据见 README 7.1。
#define BATTERY_PIN                 3       // ADC1_CH2（板载分压）
#define BATTERY_DIVIDER_RATIO       2.0f    // (R1+R2)/R2 = (100k+100k)/100k
#define BATTERY_DIODE_DROP_MV       200     // 补回肖特基压降，用万用表实测微调
#define BATTERY_CALIBRATION_FACTOR  1.000f  // 用万用表实测后校准，见 README

// 【升级方案 B】从 BAT+ 另接 100k/100k + 100nF 到 GPIO1：精度更高、插 USB 也准。
//   切换只需把上面两行改成： BATTERY_PIN 1、BATTERY_DIODE_DROP_MV 0。

#define BATTERY_SAMPLE_COUNT        16      // 每次测量的采样次数（16~32）
#define BATTERY_MAX_SAMPLES         32      // 采样缓冲区上限
#define BATTERY_SAMPLE_GAP_US       200     // 采样间隔，避免 ADC 连续转换干扰

#define BATTERY_UPDATE_INTERVAL_MS  5000    // 每 5 s 内部更新一次
#define BATTERY_NOTIFY_INTERVAL_MS  30000   // 每 30 s 至少 Notify 一次
#define BATTERY_WARN_MV             3600    // 低电量警告
#define BATTERY_CRITICAL_MV         3400    // 低于此值强制关闭激光并拒绝开启
// 低于此值认为“ADC 没接 / 悬空”，不参与低电量判断。
// 作用：还没焊分压电阻、或分压没接电池时，激光依然能正常开关，方便先做 GPIO 测试。
#define BATTERY_MIN_PLAUSIBLE_MV    2500

// ----------------------------------------------------------------- BLE ----
#define BLE_SERVICE_UUID            "7d000001-7a5b-4a86-8d3f-123456789abc"
#define BLE_CHR_CONTROL_UUID        "7d000002-7a5b-4a86-8d3f-123456789abc"  // WRITE / WRITE_NR
#define BLE_CHR_STATUS_UUID         "7d000003-7a5b-4a86-8d3f-123456789abc"  // READ / NOTIFY
#define BLE_CHR_SOC_UUID            "7d000004-7a5b-4a86-8d3f-123456789abc"  // READ / NOTIFY
#define BLE_CHR_VOLTAGE_UUID        "7d000005-7a5b-4a86-8d3f-123456789abc"  // READ / NOTIFY
#define BLE_CHR_LED_UUID            "7d000006-7a5b-4a86-8d3f-123456789abc"  // WRITE / READ（心跳灯配置）
#define BLE_CHR_ADV_UUID            "7d000007-7a5b-4a86-8d3f-123456789abc"  // WRITE / READ（广播间隔 ms，小端）

#define BLE_CMD_LASER_OFF           0x00
#define BLE_CMD_LASER_ON            0x01
#define BLE_CMD_POWER_OFF           0x02    // 关机：关激光 + 灭灯 + 进 deep sleep

// LED Config 特征的第一个字节：
#define BLE_CMD_LED_OFF             0x00    // 关心跳灯（保留时长设置）
#define BLE_CMD_LED_ON              0x01    // 开心跳灯
#define BLE_CMD_LED_FLASH           0x02    // 立刻闪一次（不改开关状态）

// Auto Sleep 特征（7d000008）的第一个字节：
#define BLE_CMD_AUTO_OFF            0x00    // 关空闲自动关机
#define BLE_CMD_AUTO_ON             0x01    // 开空闲自动关机

// ------------------------------------------------------- 心跳指示灯 ----
// 电池供电时看不出模块是否在工作，所以让板载 WS2812 每 10 s 亮一次当“心跳”。
// 默认关闭，由上位机通过 BLE 的 LED Config 特征打开并设置亮灯时长（见 indicator.h）。
#define INDICATOR_PIN                 48      // 板载 RGB（WS2812）
#define INDICATOR_NEOPIXEL            1       // 1 = WS2812 用 neopixelWrite；若板子是普通 LED 改 0
#define INDICATOR_PERIOD_MS           10000   // 心跳周期：每 10 s 闪一下
#define INDICATOR_DEFAULT_DURATION_DS 10      // 默认每次亮 1.0 s（单位 0.1 s）
#define INDICATOR_MAX_DURATION_DS     300     // 上限 30.0 s（≥周期就相当于常亮）
#define INDICATOR_COLOR_R             0       // 正常状态：绿
#define INDICATOR_COLOR_G             255
#define INDICATOR_COLOR_B             0
#define INDICATOR_LOW_COLOR_R         255     // 电池 < BATTERY_CRITICAL_MV：红
#define INDICATOR_LOW_COLOR_G         0
#define INDICATOR_LOW_COLOR_B         0

// ------------------------------------------------------- 关机 / 省电 ----
// 「关机」= 进 deep sleep（ESP32-S3 自身 ~10 µA），开机靠按 RST 或断电重上电。
//
// 关于 RST 键：它接的是芯片 EN 引脚，按下去永远是「复位」，固件分不清你是想关机
// 还是想开机。**本工程按用户要求只用 BOOT 键关机**（POWER_RST_AS_SWITCH = 0），
// RST 就单纯是「复位 / 开机」：
//   关机：BOOT 键运行中按一下，或网页点「关机」；   开机：按 RST，或断电重上电。
// 万一哪天想把 RST 也变成开关，把 POWER_RST_AS_SWITCH 改 1（原理见 power.cpp）。
#define POWER_BUTTON_PIN            0       // 板载 BOOT 键；运行中按一下 = 关机
#define POWER_BUTTON_DEBOUNCE_MS    60
#define POWER_BUTTON_IGNORE_MS      2000    // 开机后 2 s 内不认这个键（避免刚上电就误关）
#define POWER_RST_AS_SWITCH         0       // 0 = RST 只当复位/开机（用户选定的方案）
                                            //     开机只能按 RST / 断电；关机按 BOOT 键或网页
#define POWER_RST_ACCEPT_POWERON    0       // 仅当上面那行开成 1 才有意义
#define POWER_HOLD_LASER_PIN        1       // 深睡期间把激光脚钉在 LOW（安全相关，别关）

// 深睡期间按住 BOOT 键（GPIO0 拉低）当开机键：**默认关闭，别随便打开！**
// GPIO0 同时是 S3 的启动模式 strap 脚，深睡唤醒时它被拉低的话芯片会直接落进
// 串口下载模式（表现像变砖，得再按一次 RST 才能起来，见 esptool 的 Boot Mode Selection）。
// 所以开机只能用 RST / 断电。想试的人自己改 1 并自己验证。
#define POWER_BOOT_KEY_WAKE         0

// --------------------------------------------------- 空闲自动关机 ----
// 【运行时可改】BLE 特征 7d000008 / 网页「省电设置」，存在 NVS 里（键 autoEn/autoMin），
// 断电/RST/重烧不丢；下面只是出厂默认值。
// 触发条件 = 自动关机开着 + 没人连接连续 N 分钟 + 没插 USB
//（GPIO3 在 USB 供电时实测 ≈ 4.86 V、电池满电 ≈ 4.2 V，读到 > AUTO_SLEEP_USB_MV 当插着 USB）。
#define BLE_CHR_AUTO_UUID           "7d000008-7a5b-4a86-8d3f-123456789abc"  // WRITE / READ（自动关机配置）

#define AUTO_SLEEP_DEFAULT_ENABLE   1       // 出厂默认：开
#define AUTO_SLEEP_DEFAULT_MINUTES  5       // 出厂默认：无人连接 5 min -> 自动关机
#define AUTO_SLEEP_MIN_MINUTES      1
#define AUTO_SLEEP_MAX_MINUTES      1440    // 上限 24 h
#define AUTO_SLEEP_USB_MV           4600    // GPIO3 高于此值视为插着 USB（永不自动关机）

// --------------------------------------------------- BLE 省电参数 ----
// 广播/连接间隔是电池模式下最大的一块开销（射频被唤醒的次数）。
//
// 广播间隔现在是**运行时可改**的（网页上设，存在 NVS 里，RST/断电都不丢）：
// 越大越省电，越小越容易被扫到。NimBLE 内部单位是 0.625 ms。
#define ADV_INTERVAL_DEFAULT_MS     500     // 出厂默认 500 ms（够手机秒扫到）
#define ADV_INTERVAL_MIN_MS         20      // 蓝牙规范下限 20 ms
#define ADV_INTERVAL_MAX_MS         10240   // 蓝牙规范上限 10.24 s

#define BLE_CONN_INTERVAL_MIN       0x06    // 7.5 ms：本库射频常开（无控制器睡眠），连接间隔不影响功耗，
#define BLE_CONN_INTERVAL_MAX       0x18    // 30 ms  ⇒ 直接按「低延迟」选，激光连点才跟手
// 发射功率：近距（1~20 m）+3 dBm 够用，比 +9 dBm 每包省一半发射电流。
// BLE_TX_POWER_ESP 给 NimBLE 1.x / IDF 用（枚举）；BLE_TX_POWER_DBM 给 NimBLE 2.x 用（直接 dBm）。
// 两个值必须一致，改一起改。
#define BLE_TX_POWER_ESP            ESP_PWR_LVL_P3
#define BLE_TX_POWER_DBM            3
// CPU 主频：80 MHz 是运行态最省的选择（比 240 少 ~10 mA 级）。
// 代价：USB CDC 串口日志在部分主机上会掉。掉线不影响烧录（ROM 下载模式），
// 忍不了就改回 160 / 240。
#define CPU_FREQ_MHZ                80
// 同时在线客户端上限放在 platformio.ini 的 -DCONFIG_BT_NIMBLE_MAX_CONNECTIONS（当前 = 1）。

// Wi-Fi 说明：本工程从头到尾没有 include <WiFi.h>、也没有调用过任何 Wi-Fi API，
// 所以 Wi-Fi 对功耗的贡献是 0（只有占用的 Flash 空间）。别为了图方便随手加 Wi-Fi。

// ------------------------------------------------------------- 主循环 ----
#define LOOP_INTERVAL_MS            10

// -------------------------------------------------------------- 日志 ----
#define LOGF(...)   do { Serial.printf(__VA_ARGS__); } while (0)
#define LOGLN(msg)  do { Serial.println(msg); } while (0)
