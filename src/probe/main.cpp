// ============================================================================
//  电池检测引脚探针（临时诊断固件，与正式固件隔离编译）
//
//  用途：确认这块 SuperMini 到底哪个引脚能测电池，以及板载 GPIO3 分压测的是
//        “电池单体”还是“系统/VBUS 节点”。一次烧录同时读 GPIO1 / GPIO2 / GPIO3。
//
//  烧录：python -m platformio run -e adcprobe -t upload
//  日志：python -m platformio device monitor -b 115200
//  烧回正式固件：python -m platformio run -e esp32s3 -t upload
//
//  看到 “=== ADC probe ...” 开头的横幅，才说明探针真的在跑；
//  如果看到的是 [battery] / [ble] 那些日志，说明烧进去的还是正式固件。
//
//  读数怎么解读（x2 列 = 引脚电压 × 2 = 分压前那个节点的电压）：
//    · 只插电池：x2 ≈ 3.6~4.2 V   → 该引脚确实接在电池/系统电源路径上
//    · 插上 USB ：x2 ≈ 4.5~5.0 V   → 这条路径跟着 USB 走，测的不是电池单体
//    · x2 ≈ 0 V 或乱跳            → 该引脚没有分压（悬空）
// ============================================================================

#include <Arduino.h>

static const uint8_t PROBE_PINS[] = { 1, 2, 3 };   // ADC1_CH0 / CH1 / CH2
static const size_t  PROBE_PIN_COUNT = sizeof(PROBE_PINS) / sizeof(PROBE_PINS[0]);

void setup()
{
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (uint32_t)(millis() - t0) < 1500) {
        delay(10);
    }

    Serial.println();
    Serial.println("=== ADC probe: GPIO1 / GPIO2 / GPIO3 ===");
    Serial.println("对照：万用表量 B+ 对 GND 的真实电压，分别在【只插电池】和【插 USB】两种情况下记录");
    Serial.println("x2 列 = 引脚电压 x2 = 分压前节点的电压");

    for (size_t i = 0; i < PROBE_PIN_COUNT; i++) {
        analogSetPinAttenuation(PROBE_PINS[i], ADC_11db);
        pinMode(PROBE_PINS[i], INPUT);
    }
    analogReadResolution(12);
}

void loop()
{
    Serial.print("GPIO:");
    for (size_t i = 0; i < PROBE_PIN_COUNT; i++) {
        uint16_t mv = analogReadMilliVolts(PROBE_PINS[i]);
        Serial.printf("  %u=%4u mV (x2=%4u)", PROBE_PINS[i], mv, (unsigned)(mv * 2));
    }
    Serial.println();
    delay(2000);
}
