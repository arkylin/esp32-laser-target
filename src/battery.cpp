#include "battery.h"
#include "config.h"

// ---------------------------------------------------------------- 缓存 ----
static uint16_t s_millivolts     = 0;
static uint8_t  s_percent        = 0;
static bool     s_hasReading     = false;
static uint32_t s_lastUpdateMs   = 0;

// 采样缓冲区（去极值用）
static uint32_t s_samples[BATTERY_MAX_SAMPLES];

// ------------------------------------------------------- 电压 -> 电量表 ----
// 第一版近似表，最终应在“ESP32 BLE 工作 + 激光开启 + 实际电池”条件下重新标定。
struct SocPoint {
    uint16_t mv;
    uint8_t  pct;
};

static const SocPoint SOC_TABLE[] = {
    { 4200, 100 },
    { 4100,  90 },
    { 4000,  80 },
    { 3900,  65 },
    { 3800,  45 },
    { 3700,  25 },
    { 3600,  10 },
    { 3500,   5 },
    { 3400,   0 },
};
static const size_t SOC_TABLE_LEN = sizeof(SOC_TABLE) / sizeof(SOC_TABLE[0]);

// ------------------------------------------------------------------------
void initBattery()
{
    // 11 dB 衰减：量程约 0 ~ 3.1 V，分压后满电 2.1 V 落在量程中间，最合适。
    analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
    analogReadResolution(12);
}

uint16_t readBatteryMillivolts()
{
    uint32_t n = BATTERY_SAMPLE_COUNT;
    if (n < 4)                  n = 4;                    // 去极值至少要 4 个点
    if (n > BATTERY_MAX_SAMPLES) n = BATTERY_MAX_SAMPLES;

    // 1) 连续采样
    for (uint32_t i = 0; i < n; i++) {
        s_samples[i] = analogReadMilliVolts(BATTERY_PIN);
        delayMicroseconds(BATTERY_SAMPLE_GAP_US);
    }

    // 2) 排序（n <= 32，插入排序足够）
    for (uint32_t i = 1; i < n; i++) {
        uint32_t key = s_samples[i];
        int32_t  j   = (int32_t)i - 1;
        while (j >= 0 && s_samples[j] > key) {
            s_samples[j + 1] = s_samples[j];
            j--;
        }
        s_samples[j + 1] = key;
    }

    // 3) 去掉最小 1/4 与最大 1/4，对中间一半求平均
    uint32_t lo = n / 4;
    uint32_t hi = n - n / 4;
    uint32_t sum = 0;
    uint32_t cnt = 0;
    for (uint32_t i = lo; i < hi; i++) {
        sum += s_samples[i];
        cnt++;
    }
    if (cnt == 0) {
        return 0;
    }

    // 4) 换算到电池端：ADC 电压 x 分压比 x 校准系数 + 二极管压降补偿
    //    GPIO3 方案量的是肖特基之后的系统节点，把压降加回来才接近电池单体电压。
    //    GPIO1 方案（直接分压 BAT+）时 BATTERY_DIODE_DROP_MV = 0。
    float adcMv  = (float)sum / (float)cnt;
    float battMv = adcMv * BATTERY_DIVIDER_RATIO * BATTERY_CALIBRATION_FACTOR
                   + (float)BATTERY_DIODE_DROP_MV;

    if (battMv < 0.0f)      battMv = 0.0f;
    if (battMv > 65535.0f)  battMv = 65535.0f;
    return (uint16_t)(battMv + 0.5f);
}

uint8_t voltageToPercent(uint16_t millivolts)
{
    if (millivolts >= SOC_TABLE[0].mv) {
        return 100;
    }
    if (millivolts <= SOC_TABLE[SOC_TABLE_LEN - 1].mv) {
        return 0;
    }

    // 分段线性插值
    for (size_t i = 0; i + 1 < SOC_TABLE_LEN; i++) {
        uint16_t vHi = SOC_TABLE[i].mv;
        uint16_t vLo = SOC_TABLE[i + 1].mv;
        if (millivolts <= vHi && millivolts >= vLo) {
            uint8_t pHi = SOC_TABLE[i].pct;
            uint8_t pLo = SOC_TABLE[i + 1].pct;
            float t = (float)(millivolts - vLo) / (float)(vHi - vLo);
            return (uint8_t)(pLo + t * (pHi - pLo) + 0.5f);
        }
    }
    return 0;
}

bool updateBattery()
{
    uint32_t now = millis();

    // 首次调用立刻测量；之后按 BATTERY_UPDATE_INTERVAL_MS 节流。
    if (s_hasReading &&
        (uint32_t)(now - s_lastUpdateMs) < BATTERY_UPDATE_INTERVAL_MS) {
        return false;
    }
    s_lastUpdateMs = now;

    uint16_t mv  = readBatteryMillivolts();
    uint8_t  pct = voltageToPercent(mv);

    s_millivolts = mv;
    s_percent    = pct;
    s_hasReading = true;

    LOGF("[battery] %u mV, %u %%\n", mv, pct);
    return true;
}

uint16_t getBatteryMillivolts()
{
    return s_millivolts;
}

uint8_t getBatteryPercent()
{
    return s_percent;
}

bool batteryReadingPlausible()
{
    return s_hasReading && s_millivolts >= BATTERY_MIN_PLAUSIBLE_MV;
}

bool isBatteryLow()
{
    return batteryReadingPlausible() && s_millivolts < BATTERY_WARN_MV;
}

bool isBatteryCritical()
{
    return batteryReadingPlausible() && s_millivolts < BATTERY_CRITICAL_MV;
}
