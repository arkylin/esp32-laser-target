#include "ble_server.h"
#include "config.h"
#include "laser.h"
#include "battery.h"
#include "indicator.h"
#include "power.h"
#include "settings.h"

#include <NimBLEDevice.h>
#include <esp_bt.h>
#include <esp_idf_version.h>

// ---------------------------------------------------------------------------
//  NimBLE 1.x（Arduino core 2.x / IDF 4.4）与 2.x（core 3.x / IDF 5.x）兼容层。
//  两个环境共用这份源码，差异全部收在这几个宏里：
//    - 回调都多了 NimBLEConnInfo& 参数（onDisconnect 还多了 int reason）
//    - setPower 从枚举改为直接收 dBm（2.x 另有 setPowerLevel 收枚举）
//    - setScanResponse -> enableScanResponse
//    - setMinPreferred/setMaxPreferred -> setPreferredParams
// ---------------------------------------------------------------------------
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  #define NIMBLE_V2          1
  #define NIMBLE_CONN_INFO   , NimBLEConnInfo& connInfo
  #define NIMBLE_DISC_INFO   , NimBLEConnInfo& connInfo, int reason
#else
  #define NIMBLE_V2          0
  #define NIMBLE_CONN_INFO
  #define NIMBLE_DISC_INFO
#endif

// 底层连接数上限：直接取 NimBLE 的配置宏，避免和 platformio.ini 里的值写重复
#ifndef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 3
#endif
static const uint8_t BLE_MAX_CLIENTS = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;

// ============================================================================
//  本工程的 BLE 策略（按用户要求：广播模式，谁都可以控制）
//
//  1) 不配对、不绑定、无白名单：从不调用 setSecurityAuth()，特征属性也不带
//     _ENC/_AUTHEN 位 ⇒ 任何 BLE 上位机连上就能读写，不需要配对码。
//  2) 一直处于可被发现状态：客户端断开后由【主循环】重新开广播。
//     同时在线客户端上限 = CONFIG_BT_NIMBLE_MAX_CONNECTIONS（platformio.ini 里设成 1）：
//     只有一台上位机，连上之后广播就停（省电），断开后立刻恢复广播。
//  3) 多客户端同时写指令 = 后写覆盖先写（共用一个 s_pendingCmd）。
//     状态/电量通知由 NimBLECharacteristic::notify() 自动发给所有已订阅客户端。
//  4) 安全策略：只有【最后一个】客户端断开时才关激光。上限为 1 时等价于「断开即关」，
//     逻辑保留着，方便以后把上限调大。
// ============================================================================

static NimBLECharacteristic* s_statusChar = nullptr;   // Laser Status (1 B)
static NimBLECharacteristic* s_socChar    = nullptr;   // Battery SOC  (1 B)
static NimBLECharacteristic* s_voltChar   = nullptr;   // Battery mV   (2 B, little-endian)
static NimBLECharacteristic* s_ledChar    = nullptr;   // 心跳灯配置    (3 B, 见 cacheLed)
static NimBLECharacteristic* s_advChar    = nullptr;   // 广播间隔      (2 B, ms, 小端)
static NimBLECharacteristic* s_autoChar   = nullptr;   // 自动关机配置  (3 B, 见 cacheAuto)
static uint16_t              s_advMs      = ADV_INTERVAL_DEFAULT_MS;   // 运行时的广播间隔

static volatile uint8_t s_clientCount = 0;              // 当前在线客户端数
static volatile bool    s_restartAdv  = false;          // 由主循环重新开广播

// 上位机写下来的指令先存这里，由主循环统一执行（不在 BLE 回调里做决策）。
// -1 = 无指令；0 = OFF；1 = ON
static volatile int8_t s_pendingCmd = -1;

static bool     s_lastStatusValid = false;   // 有新客户端时强制同步一次状态
static uint8_t  s_lastStatus      = 0;
static uint8_t  s_lastSoc         = 0xFF;    // 0xFF = 未知，用于强制通知
static uint32_t s_lastNotifyMs    = 0;

// ------------------------------------------------------------- 内部工具 ----
static void cacheStatus(uint8_t value)
{
    s_statusChar->setValue(&value, 1);
}

static void cacheBattery(uint8_t soc, uint16_t mv)
{
    uint8_t vbuf[2] = { (uint8_t)(mv & 0xFF), (uint8_t)(mv >> 8) };
    s_socChar->setValue(&soc, 1);
    s_voltChar->setValue(vbuf, 2);
}

// 心跳灯配置的 READ 值： [enable] [durationDs 低字节] [durationDs 高字节]
static void cacheLed()
{
    bool     en = false;
    uint16_t ds = 0;
    getIndicatorConfig(&en, &ds);
    uint8_t vbuf[3] = { (uint8_t)(en ? 1 : 0), (uint8_t)(ds & 0xFF), (uint8_t)(ds >> 8) };
    s_ledChar->setValue(vbuf, 3);
}

// 广播间隔的 READ 值：uint16 ms（小端）
static void cacheAdv()
{
    uint8_t vbuf[2] = { (uint8_t)(s_advMs & 0xFF), (uint8_t)(s_advMs >> 8) };
    s_advChar->setValue(vbuf, 2);
}

// 自动关机配置的 READ 值： [enable] [minutes 低字节] [minutes 高字节]（单位：分钟）
static void cacheAuto()
{
    bool     en = false;
    uint16_t mins = 0;
    getAutoSleepConfig(&en, &mins);
    uint8_t vbuf[3] = { (uint8_t)(en ? 1 : 0), (uint8_t)(mins & 0xFF), (uint8_t)(mins >> 8) };
    s_autoChar->setValue(vbuf, 3);
}

// 把 s_advMs 真正作用到广播参数上。
// NimBLE 的单位是 0.625 ms，蓝牙规范允许 20 ms ~ 10.24 s；参数只在「开始广播」时生效，
// 所以正在广播时要重开一次（有人连着时不折腾，等他断开后由主循环重开广播自然生效）。
static void applyAdvInterval()
{
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (!pAdv) {
        return;
    }

    uint32_t units = ((uint32_t)s_advMs * 8 + 2) / 5;      // ms -> 0.625 ms 单位（四舍五入）
    if (units < 0x20)   units = 0x20;                      // 20 ms
    if (units > 0x4000) units = 0x4000;                    // 10.24 s

    pAdv->setMinInterval((uint16_t)units);
    pAdv->setMaxInterval((uint16_t)units);

    if (s_clientCount == 0) {
        pAdv->stop();
        pAdv->start();
    }

    LOGF("[ble] adv interval = %u ms (%u x 0.625 ms)\n", (unsigned)s_advMs, (unsigned)units);
}

// 让状态/电量在下一个循环立刻重发一次（新客户端接入或有人掉线后用）
static void forceResync()
{
    s_lastStatusValid = false;
    s_lastSoc         = 0xFF;
    s_lastNotifyMs    = 0;
}

// --------------------------------------------------------- BLE 回调实现 ----
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer NIMBLE_CONN_INFO) override {
        if (s_clientCount < 0xFF) {
            s_clientCount++;
        }
        LOGF("[ble] client connected (online=%u, stack=%u)\n",
             s_clientCount, (unsigned)pServer->getConnectedCount());

        forceResync();

        // 关键：连上后要重新开广播，否则第 2、3 个上位机根本扫不到本设备。
        // 交给主循环执行（回调里不直接调栈函数）。
        s_restartAdv = true;
    }

    void onDisconnect(NimBLEServer* pServer NIMBLE_DISC_INFO) override {
        if (s_clientCount > 0) {
            s_clientCount--;
        }
        s_pendingCmd = -1;

        LOGF("[ble] client disconnected (online=%u, stack=%u)\n",
             s_clientCount, (unsigned)pServer->getConnectedCount());

        if (s_clientCount == 0) {
            // 安全策略：最后一个客户端也走了 → 关激光。
            laserOff();
            cacheStatus(0);
            s_lastStatus      = 0;
            s_lastStatusValid = true;
            LOGLN("[ble] no client left -> laser OFF");
        } else {
            LOGLN("[ble] other clients still online -> laser unchanged");
            forceResync();
        }

        // 继续广播，允许随时重连（同样交给主循环执行）。
        s_restartAdv = true;
    }
};

class ControlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar NIMBLE_CONN_INFO) override {
        std::string value = pChar->getValue();
        if (value.empty()) {
            LOGLN("[ble] empty write, ignored");
            return;
        }

        uint8_t cmd = (uint8_t)value[0];
        if (cmd == BLE_CMD_LASER_ON) {
            s_pendingCmd = 1;
            LOGLN("[ble] rx 0x01 (laser ON)");
        } else if (cmd == BLE_CMD_LASER_OFF) {
            s_pendingCmd = 0;
            LOGLN("[ble] rx 0x00 (laser OFF)");
        } else if (cmd == BLE_CMD_POWER_OFF) {
            // 真正入睡的事交给主循环做（这里是 BLE 回调上下文，不能在这里动硬件/睡下去）
            s_pendingCmd = 2;
            LOGLN("[ble] rx 0x02 (power off)");
        } else {
            // 非法指令忽略，保持当前安全状态。
            LOGF("[ble] illegal cmd 0x%02X, ignored\n", cmd);
        }
    }
};

// 心跳指示灯：命令 1 字节 + 可选时长 2 字节（uint16 小端，单位 0.1 s）。
//   0x00 = 关心跳   0x01 = 开心跳   0x02 = 立刻闪一次（不改开关）
// 时长省略（只写 1 字节）或写 0 时，沿用当前配置。
class LedCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar NIMBLE_CONN_INFO) override {
        std::string value = pChar->getValue();
        if (value.empty()) {
            return;
        }

        uint8_t cmd = (uint8_t)value[0];
        uint16_t ds = (value.size() >= 3)
                        ? (uint16_t)((uint8_t)value[1] | ((uint16_t)(uint8_t)value[2] << 8))
                        : 0;

        // 这里只改 indicator 内部的变量（不碰硬件），真正点灯在主循环的 updateIndicator() 里做。
        if (cmd == BLE_CMD_LED_ON) {
            setIndicatorConfig(true, ds);
            LOGF("[led] heartbeat ON (%u ms)\n", (unsigned)(ds ? ds : INDICATOR_DEFAULT_DURATION_DS) * 100);
        } else if (cmd == BLE_CMD_LED_OFF) {
            setIndicatorConfig(false, ds);
            LOGLN("[led] heartbeat OFF");
        } else if (cmd == BLE_CMD_LED_FLASH) {
            indicatorFlash(ds);
            LOGF("[led] flash once (%u ms)\n", (unsigned)(ds ? ds : INDICATOR_DEFAULT_DURATION_DS) * 100);
        } else {
            LOGF("[led] illegal cmd 0x%02X, ignored\n", cmd);
            return;
        }

        cacheLed();   // READ 立刻反映新配置
    }
};

// 广播间隔：写 uint16 ms（小端，20 ~ 10240）；写进去就存 NVS，RST/断电后依然生效。
class AdvCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar NIMBLE_CONN_INFO) override {
        std::string value = pChar->getValue();
        if (value.size() < 2) {
            LOGLN("[ble] adv write too short, ignored");
            return;
        }

        uint16_t ms = (uint16_t)((uint8_t)value[0] | ((uint16_t)(uint8_t)value[1] << 8));
        if (ms < ADV_INTERVAL_MIN_MS) ms = ADV_INTERVAL_MIN_MS;
        if (ms > ADV_INTERVAL_MAX_MS) ms = ADV_INTERVAL_MAX_MS;

        s_advMs = ms;
        settingsPutU16(SET_KEY_ADV_MS, ms);      // 记住
        applyAdvInterval();                      // 立刻生效
        cacheAdv();
        LOGF("[ble] adv interval set to %u ms (saved)\n", (unsigned)ms);
    }
};

// 自动关机配置：1 字节模式 [0x00=关 0x01=开]，或 3 字节 [模式 + uint16 分钟（小端，1~1440）]。
// 只写 1 字节时沿用当前的超时分钟数；写进去就存 NVS，RST/断电后依然生效。
class AutoCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar NIMBLE_CONN_INFO) override {
        std::string value = pChar->getValue();
        if (value.empty()) {
            LOGLN("[ble] auto write empty, ignored");
            return;
        }

        uint8_t  mode = (uint8_t)value[0];
        uint16_t mins = 0;
        getAutoSleepConfig(nullptr, &mins);
        if (value.size() >= 3) {
            mins = (uint16_t)((uint8_t)value[1] | ((uint16_t)(uint8_t)value[2] << 8));
        }
        if (mode > BLE_CMD_AUTO_ON) {
            LOGF("[ble] auto illegal mode 0x%02X, ignored\n", mode);
            return;
        }

        setAutoSleepConfig(mode == BLE_CMD_AUTO_ON, mins);
        cacheAuto();
    }
};

// ------------------------------------------------------------------------
void initBLE()
{
    NimBLEDevice::init(DEVICE_NAME);
#if NIMBLE_V2
    NimBLEDevice::setPower(BLE_TX_POWER_DBM);          // 2.x 直接收 dBm
#else
    NimBLEDevice::setPower(BLE_TX_POWER_ESP);          // 1.x 收 IDF 的枚举
#endif

    // 广播间隔从上一次的设置里恢复（第一次开机就是 ADV_INTERVAL_DEFAULT_MS）
    s_advMs = settingsGetU16(SET_KEY_ADV_MS, ADV_INTERVAL_DEFAULT_MS);

    NimBLEServer* pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());
    pServer->advertiseOnDisconnect(true);     // 掉线后继续广播（默认即 true，写明意图）

    NimBLEService* pService = pServer->createService(BLE_SERVICE_UUID);

    NimBLECharacteristic* pControl = pService->createCharacteristic(
        BLE_CHR_CONTROL_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    pControl->setCallbacks(new ControlCallbacks());

    // 注意：属性里刻意不带 _ENC/_AUTHEN，所以任何客户端都无需配对即可访问。
    s_statusChar = pService->createCharacteristic(
        BLE_CHR_STATUS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    s_socChar = pService->createCharacteristic(
        BLE_CHR_SOC_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    s_voltChar = pService->createCharacteristic(
        BLE_CHR_VOLTAGE_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    // 心跳灯配置：WRITE 设置，READ 查询（不改硬件，主循环里点灯）
    s_ledChar = pService->createCharacteristic(
        BLE_CHR_LED_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ);
    s_ledChar->setCallbacks(new LedCallbacks());

    // 广播间隔（ms）：WRITE 设置，READ 查询；存在 NVS 里，RST/断电后不丢
    s_advChar = pService->createCharacteristic(
        BLE_CHR_ADV_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ);
    s_advChar->setCallbacks(new AdvCallbacks());

    // 自动关机配置（开关 + 超时分钟）：WRITE 设置，READ 查询；存在 NVS 里
    s_autoChar = pService->createCharacteristic(
        BLE_CHR_AUTO_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ);
    s_autoChar->setCallbacks(new AutoCallbacks());

    // 未连接时 READ 也应有正确初值（此时激光必为 OFF）。
    cacheStatus(getLaserState() ? 1 : 0);
    cacheBattery(getBatteryPercent(), getBatteryMillivolts());
    cacheLed();
    cacheAdv();
    cacheAuto();

#if !NIMBLE_V2
    pService->start();      // 2.x 里服务随 server 自动启动，start() 已废弃为空操作
#endif

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->addServiceUUID(BLE_SERVICE_UUID);
#if NIMBLE_V2
    pAdv->enableScanResponse(true);
#else
    pAdv->setScanResponse(true);
#endif
    // 连接间隔偏好：30~62.5 ms，告诉手机「别用 7.5 ms 一直把射频叫醒，也别慢到有延迟感」。
#if NIMBLE_V2
    pAdv->setPreferredParams(BLE_CONN_INTERVAL_MIN, BLE_CONN_INTERVAL_MAX);
#else
    pAdv->setMinPreferred(BLE_CONN_INTERVAL_MIN);
    pAdv->setMaxPreferred(BLE_CONN_INTERVAL_MAX);
#endif
    applyAdvInterval();          // 广播间隔（取 NVS 里的值）并开始广播

    LOGF("[ble] advertising as \"%s\" (up to %u client, no pairing)\n",
         DEVICE_NAME, (unsigned)BLE_MAX_CLIENTS);
    LOGF("[ble] preferred conn interval %u~%u ms, tx %d dBm\n",
         (unsigned)(BLE_CONN_INTERVAL_MIN * 1.25f),
         (unsigned)(BLE_CONN_INTERVAL_MAX * 1.25f), (int)BLE_TX_POWER_DBM);
}

bool isBLEConnected()
{
    return s_clientCount > 0;
}

void updateBLE()
{
    // ------------------------------------------- 0) 维持“可被发现” ----
    // 还有空闲连接位时才重开广播；已连满就等有人断开再开。
    if (s_restartAdv) {
        if (s_clientCount < BLE_MAX_CLIENTS) {
            s_restartAdv = false;
            if (NimBLEDevice::startAdvertising()) {
                LOGF("[ble] advertising restarted (online=%u/%u)\n",
                     s_clientCount, (unsigned)BLE_MAX_CLIENTS);
            } else {
                LOGLN("[ble] advertising already running");
            }
        } else {
            s_restartAdv = false;
            LOGF("[ble] at max clients (%u), stop advertising until someone leaves\n",
                 (unsigned)BLE_MAX_CLIENTS);
        }
    }

    // ---------------------------------------------------- 1) 执行指令 ----
    int8_t cmd = s_pendingCmd;
    if (cmd >= 0) {
        s_pendingCmd = -1;

        // 关机（0x02）：关激光、灭灯、进 deep sleep，不返回。
        // 放在这里执行是因为 BLE 回调跑在 NimBLE 任务上下文，不适合在那里睡下去。
        if (cmd == 2) {
            powerOff();
        }

        bool wantOn = (cmd == 1);

        // 低电量保护：临界电压下拒绝开启激光。
        if (wantOn && isBatteryCritical()) {
            LOGF("[ble] ON refused: battery %u mV < %d mV\n",
                 getBatteryMillivolts(), BATTERY_CRITICAL_MV);
            wantOn = false;
        }

        if (wantOn != getLaserState()) {
            setLaser(wantOn);
            LOGF("[laser] %s\n", wantOn ? "ON" : "OFF");
        }
    }

    bool online = (s_clientCount > 0);

    // ---------------------------------------------- 2) 激光状态 Notify ----
    uint8_t st = getLaserState() ? 1 : 0;
    if (!s_lastStatusValid || st != s_lastStatus) {
        s_lastStatus      = st;
        s_lastStatusValid = true;
        cacheStatus(st);
        if (online) {
            // 广播式通知：所有已订阅的客户端都会收到
            s_statusChar->notify();
            LOGF("[ble] notify status = %s\n", st ? "ON" : "OFF");
        }
    }

    // ------------------------------------------------ 3) 电池 Notify ----
    uint32_t now = millis();
    uint8_t  soc = getBatteryPercent();
    uint16_t mv  = getBatteryMillivolts();

    bool changed = (soc != s_lastSoc);
    bool timeout = ((uint32_t)(now - s_lastNotifyMs) >= BATTERY_NOTIFY_INTERVAL_MS);

    if (changed) {
        cacheBattery(soc, mv);   // 缓存始终刷新，保证 READ 拿到最新值
    }

    if (online && (changed || timeout)) {
        s_lastSoc      = soc;
        s_lastNotifyMs = now;
        s_socChar->notify();
        s_voltChar->notify();
        LOGF("[ble] notify battery %u %%, %u mV\n", soc, mv);
    } else if (!online) {
        s_lastSoc = soc;        // 无人连接时只更新基准，避免重连瞬间连发
    }
}
