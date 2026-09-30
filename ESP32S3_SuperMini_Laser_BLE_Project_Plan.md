# ESP32-S3 SuperMini 无线激光对靶模块方案

> 用途：机器人/授粉末端调试时的无线激光对靶模块  
> 核心功能：通过 BLE（优先）或 Wi-Fi（可选）远程控制激光开启/关闭，并返回激光当前状态、电池电压和估算电量。  
> 当前目标：先完成一个稳定、简单、可长期运行的 BLE 版本，不引入不必要的复杂度。

---

## 1. 项目目标

### 1.1 必须实现

1. ESP32-S3 SuperMini 上电后激光默认关闭。
2. 手机、Jetson、电脑或其他上位机通过 BLE 与 ESP32-S3 通信。
3. 上位机能够发送：
   - 激光开启；
   - 激光关闭；
   - 查询当前状态；
   - 查询电池信息。
4. ESP32-S3 能够返回：
   - 激光当前 ON/OFF 状态；
   - 当前电池电压；
   - 估算剩余电量百分比。
5. BLE 断开、ESP32 重启或程序异常时，激光应进入安全状态：**关闭**。
6. 代码应适合后续集成到 Jetson、Android 或 PC 控制程序。

### 1.2 后续可选功能

- Wi-Fi 控制；
- HTTP REST API；
- WebSocket/MQTT；
- OTA 固件升级；
- 板载 RGB LED 显示连接、电池、激光状态；
- 自动休眠/低功耗；
- 电量过低自动关闭激光；
- 手机端/Jetson 端图形化控制界面。

当前第一版不要同时实现全部功能，优先把 BLE 控制和电池检测做稳定。

---

# 2. 已确定硬件

## 2.1 主控板

ESP32-S3 SuperMini：

- 芯片：ESP32-S3 系列；
- 支持 2.4 GHz Wi-Fi；
- 支持 Bluetooth Low Energy；
- USB Type-C；
- 板载单节锂电池充电电路；
- 板上有 B+ / B- 电池焊盘；
- 板载 3.3 V 电源；
- 板载 WS2812/RGB LED；
- 可用 GPIO 和 ADC。

官方资料：

- ESP32-S3 SuperMini  
  https://wiki.nologo.tech/product/esp32/esp32s3/esp32s3supermini/
- 蓝牙示例  
  https://wiki.nologo.tech/product/esp32/esp32s3/esp32s3supermini/esp32S3SuperMiniBluetooth.html

原理图中可见：

- TP4054 单节锂电池充电部分；
- BAT 经肖特基二极管进入系统电源路径；
- GPIO3 被用于 `USB_IN` 检测；
- GPIO48 与板载 LED/WS2812 有关联；
- GPIO0 为 BOOT 相关引脚。

---

## 2.2 激光模块

激光模块：

https://wiki.nologo.tech/product/electronicmodule/Laserheadsensormodule.html

特征：

- 供电范围：3.3～5 V；
- 有 V / G / S 三个接口；
- S 为数字控制信号；
- 高电平开启；
- 低电平关闭；
- 模块本身已经带三极管驱动。

因此：

**不需要再额外增加 MOSFET 来驱动该激光模块。**

ESP32 GPIO 只负责给模块的 S 引脚发送逻辑控制信号。

---

# 3. 推荐总体架构

```text
           手机 / Jetson / PC
                  │
                  │ BLE
                  ▼
        ┌───────────────────┐
        │ ESP32-S3 SuperMini│
        │                   │
        │ GPIO4 ────────────┼────→ Laser S
        │ 3V3   ────────────┼────→ Laser V
        │ GND   ────────────┼────→ Laser G
        │                   │
        │ GPIO1 ← ADC分压 ←─┼──── BAT+
        └───────────────────┘
                  │
             3.7 V LiPo
```

第一版通信采用：

> **BLE GATT Server**

ESP32-S3 作为 BLE Server。

手机 / Jetson / PC 作为 BLE Client。

---

# 4. 推荐开发环境

## 4.1 最终选择

```text
VS Code
+
PlatformIO
+
Arduino Framework
+
NimBLE-Arduino
```

不推荐第一版使用：

- MicroPython；
- CircuitPython；
- ESP-IDF 原生工程。

原因不是这些方案不能实现，而是当前功能非常简单，Arduino Framework 已经足够，同时 BLE、ADC、GPIO 和后期 Wi-Fi 都容易实现。

如果后期系统演化为复杂多任务实时控制，再考虑迁移 ESP-IDF。

---

# 5. GPIO 分配

推荐如下：

| 功能 | ESP32-S3 引脚 | 说明 |
|---|---:|---|
| 激光控制 | GPIO4 | 普通数字 GPIO，推荐 |
| 电池 ADC | GPIO1 | ADC1_CH0 |
| USB 检测 | GPIO3 | 板载已有用途，不建议占用 |
| BOOT | GPIO0 | 不作为普通功能引脚 |
| 板载 RGB/LED | GPIO48 | 保留 |
| UART | U0RXD/U0TXD | 调试时尽量保留 |

## 5.1 激光控制

推荐：

```cpp
#define LASER_PIN 4
```

逻辑：

```text
GPIO4 = HIGH → 激光开启
GPIO4 = LOW  → 激光关闭
```

程序启动时必须：

```cpp
pinMode(LASER_PIN, OUTPUT);
digitalWrite(LASER_PIN, LOW);
```

即：

> **上电第一时间默认 OFF。**

---

# 6. 激光模块接线

推荐直接使用稳定的 3.3 V 电源：

```text
ESP32-S3 SuperMini      激光模块

3V3   ───────────────→  V
GND   ───────────────→  G
GPIO4 ───────────────→  S
```

优点：

- 激光电源稳定；
- 不随锂电池电压明显变化；
- USB 供电和电池供电时行为一致；
- 逻辑简单。

如果实际测试发现激光模块在 3.3 V 下亮度不足，再评估其他供电方式，但第一版优先使用 3V3。

---

# 7. 电池方案

## 7.1 电池

推荐：

- 单节 3.7 V LiPo / Li-ion；
- 容量约 500 mAh；
- **优先选择带保护板的电池**。

原因：

板子原理图中可以看到 TP4054 充电管理，但没有把它当成完整的电池过充、过放、过流保护系统。

因此推荐购买：

> 单节 3.7 V、带 PCM/保护板的锂聚合物电池。

连接：

```text
LiPo +
  │
  └──→ B+

LiPo -
  │
  └──→ B-
```

---

# 8. 电池电压检测

不要只依赖板载 GPIO3 的 `USB_IN`。

因为板载路径大致为：

```text
BAT
 │
肖特基二极管
 │
VBUS
 │
100k / 100k
 │
GPIO3
```

GPIO3 实际测到的是系统 VBUS 路径，而且插入 USB 后会变成 USB 电压，无法准确表示电池本身。

因此建议额外从 **BAT+ 直接分压到 GPIO1**。

---

## 8.1 推荐 ADC 电路

```text
BAT+
 │
100 kΩ
 │
 ├────────────→ GPIO1
 │
100 kΩ
 │
GND

GPIO1
 │
100 nF
 │
GND
```

即：

- R1 = 100 kΩ；
- R2 = 100 kΩ；
- C = 100 nF，可选但推荐；
- ADC = GPIO1。

对于 4.20 V 满电电池：

```text
Vadc = 4.20 × 100k/(100k+100k)
     ≈ 2.10 V
```

不会直接把 4.2 V 加到 ESP32 ADC。

分压网络静态电流：

```text
4.2 V / 200 kΩ ≈ 21 µA
```

对于 500 mAh 电池可以接受。

---

# 9. ADC 软件处理

建议：

```cpp
analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
```

然后使用：

```cpp
analogReadMilliVolts(BATTERY_PIN);
```

例如：

```cpp
float adcVoltage = analogReadMilliVolts(BATTERY_PIN) / 1000.0f;
float batteryVoltage = adcVoltage * 2.0f;
```

注意：

实际电阻误差、ADC 误差、PCB/导线压降都会产生偏差。

因此最终必须使用万用表校准。

建议增加：

```cpp
BATTERY_CALIBRATION_FACTOR
```

例如：

```cpp
batteryVoltage =
    adcVoltage * 2.0f * BATTERY_CALIBRATION_FACTOR;
```

---

# 10. 电池采样策略

不要单次读取 ADC 后直接返回。

推荐：

1. 连续采集 16～32 次；
2. 去掉明显异常值；
3. 求平均或中位数；
4. 每 5 秒更新一次内部电池状态；
5. BLE 查询时直接返回最近一次结果。

例如：

```text
16次 ADC
   ↓
平均
   ↓
电压
   ↓
查表
   ↓
电池百分比
```

这样返回值不会不断跳动。

---

# 11. 电池百分比计算

LiPo 电压与剩余容量不是线性关系。

不要简单使用：

```text
SOC = (V - 3.0) / (4.2 - 3.0)
```

第一版建议使用查表/分段线性插值。

可先使用近似表：

| 电压 | 估算电量 |
|---:|---:|
| ≥4.20 V | 100% |
| 4.10 V | 90% |
| 4.00 V | 80% |
| 3.90 V | 65% |
| 3.80 V | 45% |
| 3.70 V | 25% |
| 3.60 V | 10% |
| 3.50 V | 5% |
| ≤3.40 V | 0% |

这只是第一版估算。

最终应在实际：

- ESP32 BLE 工作；
- 激光开启；
- 实际 500 mAh 电池；

条件下测量并重新标定。

---

# 12. BLE 设计

推荐 BLE GATT。

## 12.1 设备名

```text
LaserTarget-S3
```

如果以后机器人有多个激光模块，可以改成：

```text
LaserTarget-L
LaserTarget-R
```

或者：

```text
LaserTarget-01
LaserTarget-02
```

---

# 13. BLE GATT 服务

推荐建立一个自定义 Service。

示例 UUID：

```text
Service UUID
7d000001-7a5b-4a86-8d3f-123456789abc
```

## 13.1 Control Characteristic

```text
UUID:
7d000002-7a5b-4a86-8d3f-123456789abc

属性:
WRITE
WRITE WITHOUT RESPONSE
```

数据：

```text
0x00 = Laser OFF
0x01 = Laser ON
```

建议不要第一版使用 `"ON"` / `"OFF"` 字符串。

二进制协议更简单、稳定。

---

## 13.2 Status Characteristic

```text
UUID:
7d000003-7a5b-4a86-8d3f-123456789abc

属性:
READ
NOTIFY
```

数据：

```text
0x00 = OFF
0x01 = ON
```

状态发生变化时立即 Notify。

---

## 13.3 Battery Characteristic

可以直接采用标准 BLE Battery Service，也可以放在自定义 Service 中。

为了第一版开发简单，可先使用自定义 Characteristic：

```text
UUID:
7d000004-7a5b-4a86-8d3f-123456789abc

属性:
READ
NOTIFY
```

数据：

```text
uint8_t
0 ~ 100
```

表示百分比。

---

## 13.4 Battery Voltage Characteristic

```text
UUID:
7d000005-7a5b-4a86-8d3f-123456789abc

属性:
READ
NOTIFY
```

建议以整数毫伏发送：

```text
uint16_t mV
```

例如：

```text
3921
```

表示：

```text
3.921 V
```

不要发送 float。

---

# 14. 推荐 BLE 数据模型

最终：

```text
Laser Control
  1 Byte

Laser Status
  1 Byte

Battery SOC
  1 Byte

Battery Voltage
  2 Byte uint16_t
```

非常轻量。

---

# 15. 状态机

内部至少维护：

```cpp
bool laserState;
bool bleConnected;

uint16_t batteryMillivolts;
uint8_t batteryPercent;
```

逻辑：

```text
BOOT
 │
 ├── Laser OFF
 │
 ├── 初始化 ADC
 │
 ├── 初始化 BLE
 │
 └── 开始广播
        │
        ▼
    BLE Connected
        │
        ├── 收到 ON
        │       ↓
        │   GPIO4 HIGH
        │       ↓
        │   Status Notify = ON
        │
        ├── 收到 OFF
        │       ↓
        │   GPIO4 LOW
        │       ↓
        │   Status Notify = OFF
        │
        └── Disconnect
                ↓
            Laser OFF
```

---

# 16. 安全策略

这是一个激光装置，因此固件必须设计成 fail-safe。

## 必须

### 16.1 上电默认关闭

ESP32 上电、复位、烧录、看门狗复位后：

```text
Laser = OFF
```

### 16.2 BLE 断开自动关闭

推荐：

```text
BLE disconnect
      ↓
Laser OFF
```

避免上位机掉线后激光一直亮。

### 16.3 非法指令忽略

只有：

```text
0x00
0x01
```

有效。

其他值：

```text
ignore
```

并保持当前安全状态。

### 16.4 低电量保护

可以设置：

```text
Battery < 3.60 V
→ Low Battery warning

Battery < 3.40 V
→ Laser OFF
```

具体阈值后续根据实际电池和负载修正。

---

# 17. BLE Notify 策略

不要持续高频发送。

推荐：

## 激光状态

仅状态发生变化时 Notify。

例如：

```text
OFF → ON
Notify

ON → OFF
Notify
```

## 电池

内部：

```text
每 5 秒测量
```

BLE Notify：

```text
每 30 秒
```

或者：

```text
电池百分比变化 ≥1%
```

时通知。

同时保留 READ，允许上位机主动查询。

---

# 18. 推荐 PlatformIO 工程结构

```text
LaserTarget/
├── platformio.ini
├── include/
│   ├── config.h
│   ├── laser.h
│   ├── battery.h
│   └── ble_server.h
│
├── src/
│   ├── main.cpp
│   ├── laser.cpp
│   ├── battery.cpp
│   └── ble_server.cpp
│
└── README.md
```

第一版如果希望尽快验证，也可以全部先写到：

```text
src/main.cpp
```

跑通以后再拆分。

---

# 19. PlatformIO 初始配置

可以先尝试：

```ini
[env:esp32s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino

monitor_speed = 115200

lib_deps =
    h2zero/NimBLE-Arduino
```

但是：

**SuperMini 的具体 Flash / PSRAM 配置要按手上实际芯片确认。**

不要盲目假设所有 ESP32-S3 DevKitC 参数都与这块 SuperMini 一致。

建议第一次连接后确认：

```text
Chip Model
Flash Size
PSRAM Size
Flash Mode
USB CDC
```

再最终固定 `platformio.ini`。

---

# 20. 软件模块设计

## 20.1 laser.cpp

职责：

```text
initLaser()
laserOn()
laserOff()
setLaser(bool)
getLaserState()
```

核心：

```cpp
void setLaser(bool on)
{
    digitalWrite(LASER_PIN, on ? HIGH : LOW);
    laserState = on;
}
```

---

## 20.2 battery.cpp

职责：

```text
initBattery()
readBatteryVoltage()
voltageToPercent()
updateBattery()
getBatteryVoltage()
getBatteryPercent()
```

要求：

- 多次采样；
- 平均；
- 校准；
- 查表估算 SOC。

---

## 20.3 ble_server.cpp

职责：

```text
BLE 初始化
Service 创建
Characteristic 创建
Advertising
连接回调
断开回调
Control Write 回调
Status Notify
Battery Notify
```

---

# 21. main.cpp 推荐逻辑

```cpp
setup()
{
    Serial.begin(115200);

    initLaser();
    laserOff();

    initBattery();

    initBLE();
}

loop()
{
    updateBatteryPeriodically();

    if (batteryTooLow()) {
        laserOff();
    }

    updateBLENotifications();

    delay(...);
}
```

Arduino ESP32 底层本身运行在 FreeRTOS 上。

第一版没有必要自行创建多个 FreeRTOS Task。

---

# 22. 上位机控制接口

## 22.1 手机

调试阶段可使用通用 BLE 工具：

- nRF Connect；
- LightBlue；
- 其他 BLE GATT 调试 App。

实现：

```text
扫描 LaserTarget-S3
↓
连接
↓
找到 Control Characteristic
↓
Write 01
↓
激光 ON

Write 00
↓
激光 OFF
```

同时查看：

```text
Status
Battery
Voltage
```

---

## 22.2 Jetson

后续推荐 Python BLE Client。

例如使用：

```text
bleak
```

控制逻辑：

```text
scan
↓
connect
↓
write Control characteristic
↓
subscribe Status notification
↓
subscribe Battery notification
```

这样很容易集成进机器人主程序。

---

# 23. Wi-Fi 第二阶段方案

当前不要和 BLE 一起做。

BLE 稳定以后，再增加 Wi-Fi。

可以提供：

```http
POST /laser/on
POST /laser/off
GET  /status
```

例如：

```json
{
  "laser": true,
  "battery_percent": 83,
  "battery_voltage_mv": 3921
}
```

但机器人末端第一版优先 BLE，因为：

- 不依赖路由器；
- 不需要 DHCP；
- 功耗更低；
- 建链逻辑更简单；
- 数据量极小；
- 当前控制需求完全够用。

---

# 24. 建议实施顺序

## Phase 1：GPIO

只连接 ESP32 + 激光。

测试：

```text
GPIO4 LOW
→ Laser OFF

GPIO4 HIGH
→ Laser ON
```

连续开关 100 次确认稳定。

---

## Phase 2：BLE 控制

实现：

```text
手机 BLE
↓
0x01
↓
Laser ON

手机 BLE
↓
0x00
↓
Laser OFF
```

同时验证：

```text
BLE disconnect
→ Laser OFF
```

---

## Phase 3：电池 ADC

焊接：

```text
BAT+ → 100k → GPIO1 → 100k → GND
                  │
                100nF
                  │
                 GND
```

对比：

```text
ESP32 ADC
vs
万用表
```

至少测试：

```text
4.2 V
4.0 V
3.8 V
3.6 V
```

得到校准系数。

---

## Phase 4：BLE 状态返回

加入：

```text
Laser Status
Battery %
Battery mV
```

---

## Phase 5：整机测试

安装：

```text
500 mAh LiPo
+
ESP32-S3 SuperMini
+
Laser Module
```

测试：

- 无线连接距离；
- 开关延迟；
- 连续工作稳定性；
- BLE 断开恢复；
- USB 充电；
- 电池续航；
- 电量显示准确性；
- 机器人运动时是否掉线；
- 电机/机械臂工作时是否有无线干扰。

---

# 25. 第一版验收指标

建议要求：

| 项目 | 目标 |
|---|---|
| BLE 连接 | 稳定 |
| 激光控制 | 100% 响应 |
| 控制延迟 | 人眼无明显延迟 |
| 上电状态 | 必须 OFF |
| BLE 掉线 | 自动 OFF |
| 状态反馈 | ON/OFF 一致 |
| 电压误差 | 校准后尽量 < ±0.05 V |
| 电量显示 | 满足状态判断即可，不追求手机级精度 |
| 连续运行 | 至少 2～4 h 无异常 |
| 重连 | 断开后可以重新连接 |
| USB充电 | 不影响正常固件运行 |

---

# 26. 本地 AI 实现时的明确约束

请按照以下条件编写代码：

1. 硬件是 **ESP32-S3 SuperMini**。
2. 使用 **PlatformIO + Arduino Framework**。
3. BLE 优先使用 **NimBLE-Arduino**。
4. 激光控制引脚暂定：
   ```cpp
   GPIO4
   ```
5. 电池检测：
   ```cpp
   GPIO1 / ADC1_CH0
   ```
6. 激光：
   ```text
   HIGH = ON
   LOW  = OFF
   ```
7. 上电后必须立即：
   ```text
   Laser OFF
   ```
8. BLE 断开必须：
   ```text
   Laser OFF
   ```
9. 电池使用：
   ```text
   BAT+ → 100k/100k → GPIO1
   ```
10. ADC 使用多次采样，不允许单次采样直接返回。
11. BLE 数据采用二进制，不使用 JSON 作为 BLE 主协议。
12. BLE 返回：
   - Laser State；
   - Battery %；
   - Battery Voltage mV。
13. 第一版不要引入：
   - Wi-Fi；
   - MQTT；
   - Web Server；
   - OTA；
   - FreeRTOS 自定义多任务；
   除非 BLE 基础版本已经稳定。
14. 所有硬件引脚和重要参数集中在：
   ```cpp
   config.h
   ```
15. BLE UUID 必须固定，不要每次生成。
16. Serial 输出必要的调试日志：
   ```text
   boot
   BLE connected
   BLE disconnected
   laser on/off
   battery voltage
   battery percent
   ```
17. 不要在主循环中大量阻塞。
18. 保持程序简单、可靠、容易修改。

---

# 27. 希望本地 AI 下一步直接生成的内容

请基于本方案生成一个可直接编译的完整 PlatformIO 工程，包括：

```text
platformio.ini

include/config.h

src/main.cpp
src/laser.cpp
src/battery.cpp
src/ble_server.cpp

对应 .h 文件

README.md
```

并实现：

```text
BLE扫描名称:
LaserTarget-S3

Control:
0x00 OFF
0x01 ON

Status:
READ + NOTIFY

Battery:
READ + NOTIFY

Battery Voltage:
READ + NOTIFY
```

另外提供一个 Python `bleak` 测试程序：

```text
scan_laser.py
laser_client.py
```

要求支持：

```bash
python laser_client.py on
python laser_client.py off
python laser_client.py status
```

并输出：

```text
Laser: ON/OFF
Battery: xx %
Voltage: x.xxx V
```

---

# 28. 当前项目最终技术路线

```text
               ┌─────────────────────┐
               │ Jetson / 手机 / PC │
               └──────────┬──────────┘
                          │
                         BLE
                          │
                          ▼
               ┌─────────────────────┐
               │ ESP32-S3 SuperMini │
               └──────┬────────┬─────┘
                      │        │
                  GPIO4     GPIO1 ADC
                      │        │
                      ▼        ▼
                激光控制    电池检测
                      │
                      ▼
                   Laser

电源：
3.7 V LiPo
    │
   B+/B-
    │
ESP32-S3 SuperMini
    │
   3V3
    │
Laser V
```

第一阶段只解决：

> **BLE 控制激光 + 状态反馈 + 电量反馈。**

不扩大功能范围。

---

# 29. 核心结论

本项目第一版推荐固定为：

> **ESP32-S3 SuperMini + Arduino Framework + PlatformIO + NimBLE-Arduino + BLE GATT + GPIO4 激光控制 + GPIO1 电池 ADC。**

激光模块自身带驱动三极管，因此无需额外 MOSFET。

电池使用单节 3.7 V LiPo，优先使用带保护板版本。

BLE 是当前场景的主通信方式，Wi-Fi 作为后续扩展，不在第一版同时实现。

整个设计原则是：

> **简单、可靠、默认安全、容易接入 Jetson/手机，并为后续机器人系统集成保留扩展能力。**
