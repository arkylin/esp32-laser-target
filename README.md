# LaserTarget-S3 — ESP32-S3 SuperMini 无线激光对靶模块

机器人/授粉末端调试用的无线激光对靶模块：通过 **BLE** 远程控制激光开/关，并回报激光状态、电池电压与估算电量。

方案文档：[ESP32S3_SuperMini_Laser_BLE_Project_Plan.md](ESP32S3_SuperMini_Laser_BLE_Project_Plan.md)（本工程完全按该文档第 26/27 节的约束实现）。

**第一版范围（不扩大）**：BLE 控制 + 状态反馈 + 电量反馈。不含 Wi-Fi / MQTT / Web Server / OTA / 自定义 FreeRTOS 任务。

---

## 1. 硬件接线

```text
        ESP32-S3 SuperMini            激光模块 (V/G/S)
        ┌───────────────┐
        │          3V3  ├────────────→ V
        │          GND  ├────────────→ G
        │      GPIO4    ├────────────→ S   (HIGH = ON, LOW = OFF)
        │               │
        │  [板载] BAT ─二极管─ 100k ─┬─ GPIO3 (ADC1_CH2)   ← 当前默认走这条
        │                           100k
        │                            │
        │                           GND
        └───────────────┘
              B+ / B-  ← 单节 3.7 V LiPo（优先带保护板），约 500 mAh

  可选升级（方案 B，精度更高、插 USB 也准）：
        BAT+ ─100k─┬─ GPIO1 (ADC1_CH0)
                  100k
                   │
                  GND
        GPIO1 ────┴─ 100nF ─ GND
```

- 激光模块自带三极管驱动，**不需要额外 MOSFET**，GPIO4 只送逻辑电平。
- 激光供电用 **3V3**（稳定、USB 与电池供电行为一致）；若实测亮度不足再评估 5 V（VBUS）方案。
- **电池检测当前用板载 GPIO3**（零焊接）：它量的是肖特基二极管之后的系统节点，所以固件用 `BATTERY_DIODE_DROP_MV` 把压降补回来；插 USB 充电时读到的是 USB 电压（≈4.9 V，电量显示 100%）。实测数据见 7.1。

### 引脚分配（`include/config.h` 集中定义）

| 功能 | 引脚 | 说明 |
|---|---:|---|
| 激光控制 | GPIO4 | 普通数字 IO，HIGH=ON |
| 电池 ADC | **GPIO3** | ADC1_CH2，板载分压（方案 A，零焊接，当前采用） |
| 电池 ADC（升级） | GPIO1 | ADC1_CH0，外接 100k/100k 分压（方案 B） |
| 板载 RGB | GPIO48 | **心跳指示灯**（WS2812），每 10 s 亮一次，默认关闭 |
| BOOT 键 | GPIO0 | 运行中按一下 = **关机**（进 deep sleep）；上电时按住则进下载模式 |
| RST 键 | EN 引脚 | 复位 = **开机**；运行中按也可当关机（默认开，见第 9 节） |

---

## 2. 工程结构

```text
ESP32/
├── platformio.ini          # 工程配置（板型/Flash/NimBLE 版本）
├── include/
│   ├── config.h            # 所有引脚、阈值、UUID、时间参数
│   ├── laser.h
│   ├── battery.h
│   ├── indicator.h         # 心跳指示灯（板载 WS2812）
│   ├── power.h             # 关机/开机（deep sleep + RTC 状态记忆）
│   ├── settings.h          # 设置持久化（NVS：心跳灯、广播间隔）
│   └── ble_server.h
├── src/
│   ├── main.cpp            # setup/loop：初始化顺序 + 低电量保护
│   ├── laser.cpp           # GPIO 开关，维护状态
│   ├── battery.cpp         # 多次采样 + 去极值 + 二极管压降补偿 + 查表算 SOC
│   ├── indicator.cpp       # 心跳灯时序：每 10 s 亮一次，默认关闭
│   ├── power.cpp           # 关机判定（RST/BOOT 键）与 deep sleep
│   ├── settings.cpp        # NVS 读写封装（Preferences）
│   ├── ble_server.cpp      # GATT Server、多客户端回调、Notify 策略
│   └── probe/main.cpp      # 电池引脚探针（独立 env:adcprobe，不进正式固件）
├── web/
│   └── index.html          # 浏览器蓝牙控制页（手机/电脑/平板通用，单文件零依赖）
├── tools/
│   ├── scan_laser.py       # 扫描并标出 LaserTarget-*
│   ├── laser_client.py     # on / off / status / monitor
│   └── serve_web.py        # 给手机用的本地 HTTPS 服务器（Web Bluetooth 要求安全上下文）
└── .consciousness/         # 项目记忆（与固件无关）
```

---

## 3. 编译与烧录

### 3.1 VS Code + PlatformIO 插件（推荐日常开发）

打开本目录即可，插件会自动安装 `espressif32` 平台与 `NimBLE-Arduino`。

### 3.2 命令行

```bash
pip install platformio          # 只需装一次
pio run                         # 编译
pio run -t upload               # 编译 + 烧录（USB Type-C 直连）
pio device monitor              # 串口日志，115200
```

首次连接后，串口启动日志里确认芯片实际参数，再回来改 `platformio.ini`：

```text
Chip Model / Flash Size / PSRAM Size / Flash Mode
```

默认按 **4 MB Flash、无 PSRAM** 配置；若手上是 ESP32-S3FH4R2（带 2 MB PSRAM），把 `platformio.ini` 里那两行 PSRAM 注释打开。

> `platformio.ini` 里的 `platform = espressif32@^6.9.0` 对应 Arduino core 2.x，保证 `ADC_11db` 等 API 可用。要迁到 core 3.x 见文件末尾注释。

### 3.3 首次烧录 + 首检（照着做）

```powershell
# 0) 只在需要下载依赖时设代理（首次装平台/库时）
$env:HTTP_PROXY="http://127.0.0.1:7897"; $env:HTTPS_PROXY="http://127.0.0.1:7897"

# 1) 插上 Type-C，列出串口，记下 COMx
python -m platformio device list

# 2) 编译 + 烧录（只有一个串口时自动选；多个串口时显式指定）
python -m platformio run -t upload
python -m platformio run -t upload --upload-port COM5

# 3) 看日志
python -m platformio device monitor -b 115200
```

> 本机 `pio` 不在 PATH，所以统一用 `python -m platformio`。

**开机日志应该是这样**（数值随实际变化）：

```text
=== LaserTarget-S3 boot ===
[laser] GPIO4 = OFF (safe default)
[battery] 4021 mV, 80 %
[ble] advertising as "LaserTarget-S3"
=== ready ===
```

**还没焊 100k/100k 分压时**，GPIO1 悬空会读到很低的假值，串口会打印一行 `[battery] xx mV 不可信…`。固件把 `< 2.5 V` 当作“ADC 未接”，此时低电量保护不生效，**激光照常可以开关**——方便先做 Phase 1 的 GPIO 测试。

**烧不进去**：按住 **BOOT(GPIO0)** → 点按一下 **RST** → 松开 BOOT，进入下载模式后再执行 upload。
**枚举不出串口**：ESP32-S3 原生 USB 在 Win10/11 免驱，设备管理器里应看到 `USB 串行设备 (COMx)` 或 `USB JTAG/serial debug unit`；若什么都没有，先换一根**能传数据**的 Type-C 线（很多线只供电）。
**烧录后串口号变了**：固件跑起来用的是 USB-OTG  CDC，下载模式用的是 ROM 的 USB-Serial-JTAG，两者 COM 号可能不同；监视器里选当前那个。

---

## 4. BLE 协议

设备名：**`LaserTarget-S3`**（多模块可改 `LaserTarget-L/R` 或 `-01/-02`，改 `config.h` 的 `DEVICE_NAME`）

Service：`7d000001-7a5b-4a86-8d3f-123456789abc`

| Characteristic | UUID | 属性 | 数据 |
|---|---|---|---|
| Control | `7d000002-…-123456789abc` | WRITE / WRITE_NR | `0x00` = 关激光，`0x01` = 开激光，`0x02` = **关机**（关激光+灭灯+进 deep sleep）；其它值忽略 |
| Status | `7d000003-…-123456789abc` | READ / NOTIFY | `uint8` 0 = OFF，1 = ON |
| Battery SOC | `7d000004-…-123456789abc` | READ / NOTIFY | `uint8` 0~100 (%) |
| Battery Voltage | `7d000005-…-123456789abc` | READ / NOTIFY | `uint16` mV，**小端**，如 `0x51 0x0F` = 3921 = 3.921 V |
| LED Config | `7d000006-…-123456789abc` | WRITE / READ | 写：`cmd` + 可选 `uint16` 时长（0.1 s 单位，小端）；读：`enable` + `uint16` 时长 |
| Adv Config | `7d000007-…-123456789abc` | WRITE / READ | `uint16` 广播间隔 ms（小端，20 ~ 10240） |
| Auto Config | `7d000008-…-123456789abc` | WRITE / READ | 写：`mode`（`0x00`=关 / `0x01`=开）+ 可选 `uint16` 分钟（小端，1~1440）；读：`enable` + `uint16` 分钟 |

LED Config 的 `cmd`（心跳指示灯，见 [第 5.1 节](#51-网页控制webindexhtml免装-app推荐)）：

| cmd | 含义 |
|---|---|
| `0x00` | 关心跳灯（时长仍会保存） |
| `0x01` | 开心跳灯 |
| `0x02` | **立刻闪一次**（不改开关状态，用来确认灯亮不亮） |

时长省略（只写 1 字节）、或写 0 时沿用设备里的当前值；单位 0.1 s，上限 30 s（`INDICATOR_MAX_DURATION_DS`）。

**LED Config、Adv Config 与 Auto Config 都会写进 NVS**（`include/settings.h`，命名空间 `laser`，键 `ledEn`/`ledDs`/`advMs`/`autoEn`/`autoMin`）：按 RST、断电重上电、重新烧固件（不 `erase_flash`）之后依然有效。只有 `pio run -t erase` 或整片擦除才会回到默认值（心跳灯默认关、广播间隔 500 ms、自动关机默认开/5 分钟）。

Notify 策略：

- **状态**：只在 ON↔OFF 变化时发（外加连上后立刻同步一次）。
- **电池**：电量变化 ≥1% 或距上次 ≥30 s 时发；同时保留 READ 供主动查询。

### 无需配对 / 同时在线 1 台（广播模式）

- **不用配对**：固件从不调用 `setSecurityAuth()`，特征属性也不带 `_ENC/_AUTHEN` 位 ⇒ **任何** BLE 上位机连上即可读写，无配对码、无白名单。
- **同时只允许 1 台**（按用户要求，为了省电）：`platformio.ini` 里 `-DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`。
  - 连上后固件**停止广播**（省电），第二台设备此时扫不到它；断开后主循环立刻恢复广播。
  - 想恢复「手机 + 电脑同时在线」，把这个值改回 `3` 即可，多客户端逻辑一直都在。
- **掉线安全**：客户端断开就关激光（上限为 1 时等价于「断开即关」）；状态/电量通知发给所有已订阅客户端。

---

## 5. 手机 / 电脑 / 平板调试

### 5.1 网页控制（`web/index.html`，免装 App，推荐）

单文件、零依赖、离线可用，用 **Web Bluetooth** 直连设备，界面就是一个大开关 + 状态/电量显示。

| 用哪台设备 | 怎么打开 |
|---|---|
| 任何设备（推荐） | **在线版**：<https://arkylin.github.io/esp32-laser-target/web/>（GitHub Pages 托管，https 满足 Web Bluetooth 的安全上下文要求；push 到 main 自动更新） |
| Windows / macOS / Linux 电脑 | Chrome 或 Edge **直接双击** `web/index.html` 即可（`file://` 也算安全上下文），离线可用 |
| 安卓手机 / 平板 | 打开上面在线版即可；没有网时用下面「手机怎么打开」的本地 HTTPS |
| iPhone / iPad | **Safari 不支持 Web Bluetooth**，装 **Bluefy** 浏览器打开同一页面（在线地址同样适用） |

**安卓 / 手机怎么打开**（浏览器只允许 https 页面调用蓝牙）：

```powershell
# 一次性生成自签证书（Git for Windows / Linux / macOS 自带 openssl）
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 `
    -keyout tools/key.pem -out tools/cert.pem -subj "/CN=laser-web"

python tools/serve_web.py          # 打印 https://192.168.x.x:8443/
```

手机连**同一个 Wi-Fi**，Chrome 打开上面打印的地址；证书不受信任时点「高级 → 继续访问」，之后就正常了。
不想用自签证书的备选：手机 Chrome 打开 `chrome://flags/#unsafely-treat-insecure-origin-as-secure`，把 `http://<电脑IP>:8000` 加进去，然后随便用 `python -m http.server` 起个 http 服务也能用。

页面行为：

- 点「连接蓝牙」→ 选择 **LaserTarget-S3**（按名字前缀 + 服务 UUID 两种过滤，改过名字也能选到）。
- 大开关写 `0x01/0x00`；状态、电量、电压走 Notify 实时刷新，另在 0.9 s 后主动 READ 一次兜底。
- **支持快速连点**：手指「按下」就发指令（不等抬起）；连点时 250 ms 内到达的旧回报不回灌 UI、不误报「设备没有执行」；0.9 s 的兜底回读做了防抖，连点时只保留最后一次。固件侧连接间隔请求 7.5~30 ms（射频常开时间隔不耗电，纯为延迟服务）——按下到激光翻转典型 ~20~60 ms。
- 手机切后台被系统断链时会自动重连 4 次（退避 0.8~3.2 s）；点「断开连接」则不再重连。
- 底部有日志；`web/index.html?selftest=1` 会跑一遍协议解析自检（7 项）。
- **心跳指示灯卡片**：勾「开启」+ 填「每次亮 N 秒」即可让板载 RGB 每 10 s 亮一次（电池供电时用来看模块还在不在）；「立即闪一次」不用等满一个周期就能确认灯是好的。**默认关闭**，设置写进设备 NVS —— 按 RST、断电重上电之后再连上，页面会自动把这两个值读回来。
- **省电设置卡片**：广播间隔（ms）随手改，越大越省电、越小越容易被扫到（20 ~ 10240，默认 500）；**无人连接自动关机**（开关 + 1~1440 分钟，默认 5）——电池供电、没人连接超过设定时间就自动深度睡眠，插着 USB 永不触发（调试/充电不打扰），详见 [9.4](#94-空闲自动关机2026-09-29-新增)。都存 NVS，RST/断电后仍生效。
- **「关机」按钮**（确认框挡一下误触）：写 Control `0x02`，设备关激光 → 灭灯 → 进 deep sleep，蓝牙随即断开；之后要按板子上的 **RST** 才能再开机。
- **自动找回设备**：浏览器还记着上次授权时（Chrome 的持久化权限），打开页面会自动连回上次那台 `LaserTarget-*`，不用再点一次选择框；没有权限就照旧手动点「连接蓝牙」。也可以手动再点一次——不如说**只有 1 台能连**：第二台设备在有人连着时扫不到本机。
- **5 种主题**：跟随系统 / 深色 / 浅色 / 高对比 / 夜间（OLED 纯黑+暗红），右上角下拉切换，存 `localStorage`，刷新不丢；也可用 `?theme=light` 临时指定。
- 手机、电脑**不能同时**连（上限 1 台，见上面「自动找回设备」）。

### 5.1.1 心跳指示灯（电池供电时看模块是否在工作）

插着 USB 时有充电灯，纯电池供电就什么灯都没有，分不清模块是「在工作」还是「没电了/死机」。所以：

```text
    |<------------- 10 s（INDICATOR_PERIOD_MS）------------>|
    |####|..................................................|####|...
     ↑ 亮 N 秒（默认 1.0 s，0.1~30 s 可调）
```

- **默认关闭**：每次上电/复位都是灭的，必须由上位机（网页勾选 / nRF Connect 写 LED Config 特征）打开，和激光的「安全默认」一个思路。
- 颜色：正常**绿**；电池低于 `BATTERY_CRITICAL_MV`（3.40 V）时改**红**闪，不用掏手机就知道该充电。
- 参数在 `include/config.h`：`INDICATOR_PIN`（48）、`INDICATOR_PERIOD_MS`（10000）、`INDICATOR_DEFAULT_DURATION_DS`（10 = 1.0 s）、`INDICATOR_MAX_DURATION_DS`（300 = 30 s）、两个颜色。**开关和时长本身是运行时可改的**（网页 / BLE 特征 `7d000006`），并存进 NVS。
- 灯是**板载 WS2812**，用 Arduino core 自带的 `neopixelWrite()`，不额外引库。万一这块板的 LED 是普通 LED 而不是 WS2812，把 `INDICATOR_NEOPIXEL` 改成 `0`（改用 `digitalWrite`）即可。
- 省电：只在「亮↔灭」翻转的那一次写 LED，不会每 10 ms 去刷 RMT。

### 5.2 nRF Connect / LightBlue（原始 GATT 调试）

```text
扫描 LaserTarget-S3 → 连接
→ 找到 Service 7d000001-…
→ Control 特征 Write 01 → 激光 ON，Status 变 1
→ Control 特征 Write 00 → 激光 OFF，Status 变 0
→ 读 Status / Battery SOC / Battery Voltage 查看回传
→ LED Config 特征 Write `01 0A 00` → 心跳灯开启、每次亮 1.0 s（后两字节可省）
→ LED Config 特征 Write `02`       → 立刻闪一次（确认板载灯是好的）
→ Adv Config 特征 Write `F4 01`     → 广播间隔改成 0x01F4 = 500 ms（写进去即存 NVS）
→ Control 特征 Write `02`           → 关机（进 deep sleep，按 RST 再开机）
```

---

## 6. 上位机 Python 工具（bleak）

```bash
pip install bleak

python tools/scan_laser.py            # 扫到设备（标 <== LASER）
python tools/laser_client.py on       # 开激光
python tools/laser_client.py off      # 关激光
python tools/laser_client.py status   # 查状态 + 电量
python tools/laser_client.py monitor  # 订阅通知持续打印
python tools/laser_client.py stress --count 100   # Phase 1：连续开关 100 轮并核对每次状态
```

`status` 输出：

```text
Laser: ON
Battery: 83 %
Voltage: 3.921 V
```

支持 `--address AA:BB:CC:DD:EE:FF` 跳过扫描，`--name` 改名，`--timeout` 改超时。
Jetson 上直接把这套 `bleak` 调用嵌进机器人主程序即可。

---

## 7. 电池标定（Phase 3，必做）

### 7.1 电池检测走哪个引脚（当前采用：板载 GPIO3）

厂商资料/客服会说「接了外部电池后，采集 **GPIO3** 的模拟电压即可测电量」——这话没错，但要分清它测的是**哪个节点**：

| | 板载 GPIO3 这条路 | 本方案 GPIO1 + 外接 100k/100k |
|---|---|---|
| 硬件成本 | 板子已焊好，零成本 | 2 个电阻 + 1 个电容 |
| 测的是什么 | 系统/VBUS 节点（BAT 经肖特基二极管之后的路径） | 电池单体 BAT+ 本身 |
| 插 USB 时 | 跟着 USB 走（约 4.7 V），完全无法表示电池 | 不受影响 |
| 只插电池时 | 比电池低一个二极管压降（约 0.2~0.3 V，且随负载变化） | 就是电池电压 |
| 引脚其它身份 | ESP32-S3 的 **strapping 脚**（上电决定 JTAG 来源），板载已占用 | 普通 ADC 脚，干净 |

结论：**要“大概有电没电”** → GPIO3 零焊接够用；**要方案里写的 ±0.05 V、且插 USB 充电时也准** → 用 GPIO1 + 外接分压。

**当前 `config.h` 采用方案 A（GPIO3）**：`BATTERY_PIN = 3`、`BATTERY_DIODE_DROP_MV = 200`。方案 B 的切换口已留好，改两行即可（见 7.2 末尾）。

> **本机实测（2026-09-28，探针固件，只插 USB、未接电池）**：
> `GPIO3 = 2432 mV (×2 = 4864 mV)` —— 没有电池却读到 4.86 V，而锂电池上限只有 4.2 V，5 V 减去一个肖特基压降正好是这个数。
> **⇒ 这块板的 GPIO3 分压接在 USB/VBUS 节点，测不了电池**（只能当 USB 插入检测）。
> 同时 `GPIO1 = 576 mV`、`GPIO2 = 517 mV` 缓慢漂移 = 悬空引脚（不是 0），所以 `BATTERY_MIN_PLAUSIBLE_MV` 这道门槛是必需的。
> 换一块板想快速判断，方法一样：**不接电池时读一次，> 4.2 V 就是跟 USB 走**。

别猜，实测。工程里带了一个独立的探针固件，一次烧录同时读 GPIO1/2/3：

```powershell
python -m platformio run -e adcprobe -t upload    # 烧探针（不碰激光）
python -m platformio device monitor -b 115200     # 同时看三个引脚的 ADC
```

分别在【只插电池】和【插 USB】两种情况下记录读数，按 [src/probe/main.cpp](src/probe/main.cpp) 顶部的说明解读。测完烧回正式固件：

```powershell
python -m platformio run -e esp32s3 -t upload
```

> 探针跑起来的标志是串口出现 `=== ADC probe: GPIO1 / GPIO2 / GPIO3 ===` 横幅。**如果看到的是 `[battery]` / `[ble]` 日志，说明烧进去的还是正式固件**——先对比 `.pio/build/adcprobe/firmware.bin` 与 `.pio/build/esp32s3/firmware.bin` 的大小是否不同（正常约 275 KB vs 523 KB）。

选定之后只需改 `config.h` 一行 `BATTERY_PIN`（用 GPIO3 就填 `3`），必要时再调 `BATTERY_DIVIDER_RATIO`。

### 7.2 校准（接上电池之后）

方案 A（GPIO3）的误差有两部分，先修**加性**的二极管压降，再修**乘性**的分压/ADC 误差：

1. 烧录后打开串口，看到 `[battery] xxxx mV, xx %`（`V_read`）。
2. 用万用表量 **BAT+ 与 GND** 之间的实际电压（`V_real`）。
3. 修加性偏差：

   ```text
   BATTERY_DIODE_DROP_MV_new = BATTERY_DIODE_DROP_MV_old + (V_real − V_read)
   ```

   例：串口 `3700 mV`、万用表 `3.90 V` → `200 + 200 = 400`。
4. 修乘性偏差：

   ```text
   BATTERY_CALIBRATION_FACTOR_new = FACTOR_old × (V_real / V_read)
   ```

5. 两个值写回 `config.h`，重烧，至少核对 **4.2 / 4.0 / 3.8 / 3.6 V** 四点。

方案 B（GPIO1）只有乘性误差：`BATTERY_DIODE_DROP_MV = 0`，只用第 4 步的系数即可，能做到 **< ±0.05 V**。

方案 A 的先天限制：插着 USB 时 GPIO3 读到的是 USB 电压（≈4.9 V），所以**充电期间误差必然大**，那时电量显示 100% 就是“正在充电”的意思；只有不插 USB、纯电池工作时才谈得上精度。

> 只想切到方案 B：把 `config.h` 里 `BATTERY_PIN` 改成 `1`、`BATTERY_DIODE_DROP_MV` 改成 `0`，其余代码不用动。

电量百分比用查表 + 分段插值（见 `src/battery.cpp` 的 `SOC_TABLE`），后续按实际放电曲线修正。

---

## 8. 安全策略（fail-safe）

| 场景 | 行为 |
|---|---|
| 上电 / 复位 / 烧录 / 看门狗复位 | 激光 OFF（`initLaser()` 先拉低再设输出，避免毛刺）；心跳指示灯也是关的（`initIndicator()` 物理熄灭 WS2812） |
| BLE 断开（**最后一个**客户端） | 立即 OFF，并继续广播等待重连；还有其它客户端在线时不动激光 |
| 非法指令（非 0x00/0x01） | 忽略，保持当前安全状态，串口打印告警 |
| 电池 < 3.60 V | 低电量警告（串口，最多 60 s 一次） |
| 电池 < 3.40 V | 强制关闭激光，且**拒绝**上位机的开启指令 |
| 关机（BOOT 键 / 网页 `0x02` / RST） | 先关激光 → 灭指示灯 → 把激光脚保持成 LOW → 进 deep sleep；开机后仍是「激光 OFF」的安全默认 |

阈值都在 `config.h`：`BATTERY_WARN_MV` / `BATTERY_CRITICAL_MV`。

---

## 9. 关机与省电（电池模式）

### 9.1 关机 / 开机

「关机」= 进 **deep sleep**：激光 OFF、指示灯灭、蓝牙和 CPU 全停，等按 RST 或断电才醒。

| 动作 | 怎么做 |
|---|---|
| **关机** | ① 板载 **BOOT 键**运行中按一下（**本工程采用的方式**，和复位无关、一定好使）；② 网页点「关机」（写 Control `0x02`） |
| **自动关机** | 电池供电 + **无人连接连续 5 min**（`AUTO_SLEEP_IDLE_MS`）→ 自动进 deep sleep；**插着 USB 永不触发**（开发/充电/台架演示不打扰），见 9.4 |
| **开机** | 按 **RST**，或断电重上电（**BOOT 键当不了开机键**，原因见下） |

**为什么不能用 BOOT 键开机**：GPIO0 同时是 S3 的**启动模式 strap 脚**。深睡唤醒时如果它还处于低电平（手正按着 BOOT），芯片会直接落进**串口下载模式**——表现就是"按了 BOOT 屏幕没反应、串口里是 ROM boot 信息"，得再按一次 RST 才能起来（参考 esptool 的 [Boot Mode Selection](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/advanced-topics/boot-mode-selection.html)）。所以固件里 `POWER_BOOT_KEY_WAKE` **默认 = 0**，开机只能用 RST / 断电。

**为什么不用 RST 当开关**：RST 键接的是芯片 EN 引脚，按下去永远只是「复位」——固件分不清你是要关机还是要开机。所以本工程按用户选择：**RST 只当复位/开机，关机交给 BOOT 键**（`config.h` 里 `POWER_RST_AS_SWITCH = 0`，这条代码路径还在，随时能改回 1）。

不过「上次是开还是关」这个状态仍然记在 RTC 域里（掉电才丢），用来做兜底：

```text
上电复位 (POWERON)        -> 一定开机（绝不会停在关机态）
标记 = 上次是关机          -> 开机（BOOT 键关机后按 RST 就是走这条）
崩溃 / 看门狗 / 烧录复位   -> 一定开机（fail-safe：异常复位不会变成关机）
```

开机串口日志会把判定过程打出来：

```text
[power] reset reason = 1 (POWERON 上电), rtc state = ON
[power] POWERON reset -> power ON (POWER_RST_ACCEPT_POWERON=0)
[power] BOOT button pressed -> shutdown
[power] deep sleep now - press RST (or power cycle) to power on
```

- 开机后 2 s 内不认 BOOT 键（`POWER_BUTTON_IGNORE_MS`），免得手还按着就被关掉；按满 60 ms 才算一次（`POWER_BUTTON_DEBOUNCE_MS`）。
- 深睡前会把**激光脚保持成 LOW**（`POWER_HOLD_LASER_PIN`）。很关键：深睡时 GPIO 变高阻，激光模块的 S 脚一旦悬空被拉高就会一直亮着。
- 关机时不调 `NimBLEDevice::deinit()`：深睡直接断掉数字域，在 BLE 上下文里操作协议栈反而有死锁风险。
- 想让 RST 也变成开关：把 `POWER_RST_AS_SWITCH` 改 `1`（原理与两个旋钮见 `src/power.cpp` 顶部注释）。

### 9.2 省电清单（到底哪儿在耗电）

**先说一个查证过的事实**（决定了下表里几行旋钮的真实收益）：

> Arduino core 2.0.17 的 BLE 库是**预编译**的，控制器睡眠被关死：
> `%USERPROFILE%\.platformio\packages\framework-arduinoespressif32\tools\sdk\esp32s3\sdkconfig` 里
> `CONFIG_BT_CTRL_SLEEP_MODE_EFF=0`、`CONFIG_PM_ENABLE` 未开、tickless idle 未开。
> 含义：**只要 BLE 栈活着，射频就 100% 通电常开**——广播间隔、连接间隔只影响"射频醒来的节奏"，
> 对平均电流几乎没有影响；CPU 空闲时也只是全速空转。想打破这个下限见 9.5。

| 项目 | 现状 | 量级参考 | 怎么改 |
|---|---|---:|---|
| **Wi-Fi** | **全程从未初始化**（第一版范围就排除了） | **0**（只占 Flash） | 不用改；别随手 `#include <WiFi.h>` 再 `begin()` |
| **运行态整体（BLE 活着）** | 射频常开 + CPU 空转 | **~40~60 mA**（240 MHz）；**80 MHz 后 ~35~45 mA** | 大头在下两行 + 9.5 |
| 蓝牙广播 | 默认 500 ms（网页可调 20 ms~10.24 s，存 NVS） | 广播间隔**几乎不省电**（射频常开，见上） | 间隔只当"可被发现速度"旋钮用：大 = 难扫到，小 = 秒扫到 |
| 蓝牙已连接 | 期望连接间隔 **7.5~30 ms**（`BLE_CONN_INTERVAL_MIN/MAX`） | 同上，间隔不影响平均电流 | 按「低延迟」选（激光连点跟手）；想压电流只能靠 9.5 的控制器睡眠 |
| 蓝牙**同时**连接多台 | **已限制 1 台**（`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`）：连上就停广播 | 每多一条连接多一份开销 | 要多台改回 3 |
| 发射功率 | **已设 +3 dBm**（`BLE_TX_POWER_ESP` / `BLE_TX_POWER_DBM`，两值必须一致） | 比原来 +9 dBm 每包省一半发射电流 | 近距再降 `ESP_PWR_LVL_P0` / `BLE_TX_POWER_DBM 0` |
| CPU 主频 | **已设 80 MHz**（`CPU_FREQ_MHZ`） | 比 240 MHz 少 ~10 mA 级 | USB CDC 日志若掉线改回 160/240（不影响烧录） |
| USB CDC 日志 | 开（`ARDUINO_USB_CDC_ON_BOOT=1`） | **待实测**（怀疑几 mA 级） | 现场版可关，见下 |
| 心跳指示灯 | 默认关 | 亮时绿 ~7 mA、三色全亮 ~20 mA | 关着，或把时长调短 |
| WS2812 静态电流 | 不亮也在耗 | ~0.3~1 mA | 芯片特性，软件关不掉；长期存放拔电池 |
| 电池分压电阻 | 100k/100k 常年挂着 | ~21 µA（4.2 V ÷ 200 kΩ） | 深睡也耗；硬件升级换 1M/1M + 100 nF |
| **深睡（关机）** | 本次新增**空闲自动关机**（9.4） | ESP32-S3 自身 ~7~10 µA；**板上大头是 LDO、WS2812 静态电流、分压电阻** | 就是 9.1 |

**关于 Wi-Fi**：本工程从头到尾没有 `#include <WiFi.h>`、没调用任何 Wi-Fi API（`grep WiFi` 可以自查），所以它对功耗的贡献是 0，**不用特意去关**。射频（PHY）是给蓝牙用的，关不掉。

**关于多台设备同时连接**：会明显更费电，所以本工程**默认只允许 1 台**（`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`）——连上之后固件直接停止广播，比"一直广播"更省。要手机和电脑同时在线，把那个值改回 3 即可。

**关掉 USB 日志（现场版，收益待实测）**：`platformio.ini` 里加

```ini
[env:field]
extends = env:esp32s3
build_flags = ${env:esp32s3.build_flags} -DARDUINO_USB_CDC_ON_BOOT=0
```

这样 `Serial` 变成 UART0（这块板没引出 UART 引脚 ⇒ 等于没有日志），重新烧录仍走 ROM 下载模式（BOOT+RST）不受影响。**先用电流表对比两个固件再决定值不值得牺牲串口日志。**

### 9.3 怎么量（实测协议）

**工具**：USB 功率计（带 mAh/mWh 累计的最好用，BLE 射频是脉冲负载，万用表读数会乱跳）；万用表 µA 档可以量深睡态。串在电池正极与 BAT+ 之间，或只插 USB 量（此时 GPIO3 读到的是 USB 电压、电量显示 100%，属正常，不影响测电流）。

**量这五个状态**，每个稳定 1~2 min 记平均值：

| # | 状态 | 怎么进 | 预期量级 |
|---|---|---|---|
| ① | 运行态·广播中 | 只上电，不连接 | ~35~45 mA（80 MHz） |
| ② | 运行态·已连接 | 手机/网页连上，激光 OFF | 与 ① 差别不大（射频常开） |
| ③ | 运行态·激光 ON | 连接后开激光 | ① + 激光模块电流（20~30 mA 量级） |
| ④ | 关机态（深睡） | 按 BOOT 或网页关机 | 几十 µA（芯片 ~10 µA + 分压 21 µA + LDO/WS2812） |
| ⑤ | 广播间隔影响 | 同 ①，网页上把广播间隔改 20 ms / 10.24 s 各量一次 | **应该几乎一样**（射频常开）；若 ⑤ 比 ① 明显省，说明你的库其实带睡眠，把结论改回来 |

**决策点**：
- ④ 明显大于 ~100 µA → 板级漏电（WS2812 暗电流/分压/LDO）比芯片大一个量级以上，优先按 9.2 的硬件项改；
- ①②③ 是耗电主力 → 续航优化 = 缩短 ①②③ 的总时长 = **靠 9.4 的自动关机**，而不是抠 ①②③ 的电流；
- 电池续航粗算：`电池 mAh ÷ 平均 mA = 小时数`（例如 1000 mAh、平均 40 mA ≈ 25 h；平均 1 mA ≈ 41 天）。

### 9.4 空闲自动关机（2026-09-29 新增，网页/BLE 运行时可改）

`updateAutoSleep()`（`src/power.cpp`）在主循环里每 10 ms 检查一次，**三个条件同时满足**才自动关机（进 deep sleep，路径与按 BOOT 键关机完全一致，安全逻辑全复用）：

1. 开关打开（**默认开**）；
2. **没有任何 BLE 客户端连接**连续超过设定分钟数（**默认 5 min**，可改 1~1440）；
3. **没插 USB**：GPIO3 实测 USB 供电 ≈ 4.86 V、电池满电 ≈ 4.2 V，读到 ≥ `AUTO_SLEEP_USB_MV`（4600）就当插着 USB —— 开发调试、充电、台架演示永不误触发。

**怎么改**（运行时设置，存 NVS，键 `autoEn`/`autoMin`，RST/断电/重烧不丢）：

| 渠道 | 操作 |
|---|---|
| 网页 | 「省电设置」卡片：勾「无人连接自动关机」+ 填分钟数，改动立即下发并保存 |
| BLE | 写 Auto Config 特征 `7d000008`：`[0x00/0x01, 分钟低, 分钟高]`（分钟可省略=沿用当前值）；READ 返回 `[enable, 分钟低, 分钟高]` |
| 出厂默认 | `config.h` 的 `AUTO_SLEEP_DEFAULT_ENABLE` / `AUTO_SLEEP_DEFAULT_MINUTES`；上限 `AUTO_SLEEP_MAX_MINUTES`（1440 = 24 h） |

细节：

- 有人连着就永不超时（哪怕激光一直亮着也不会睡——但正常逻辑下激光亮必有客户端，客户端走了激光已经被关）；
- 改配置视为一次活动，空闲计时从头起算；
- 醒来（按 RST / 断电）走原有开机判定，一律判成「开机」，fail-safe 不变；
- 触发时串口打 `[power] no BLE client for 5 min on battery -> auto shutdown`。

### 9.5 运行态功耗下限：为什么省不下来 & `esp32s3_next` 路线

**为什么现在压不进毫安级**：运行态电流 ~40 mA 里，射频常开占 ~25~35 mA，而射频能不能"睡着"是**编译期**决定的（见 9.2 顶部的 sdkconfig 事实），Arduino core 2.x 的预编译库不带控制器睡眠，运行时没有任何 API 能打开它。

**`esp32s3_next` 环境**（`platformio.ini`，实验性）：Arduino core 3.3.12（IDF 5.5）+ NimBLE-Arduino 2.x 的升级路线，源码共用（`ble_server.cpp` 顶部 `NIMBLE_V2` 兼容层）。烧录：`python -m platformio run -e esp32s3_next -t upload`。注意三点：
1. 与主环境共用全局包目录里的 `framework-arduinoespressif32`（版本不同），两个环境来回编译会各触发一次重装（有缓存，1~2 min），不是坏了；
2. **3.x 的预编译库同样不带 BLE 控制器睡眠（已查证）**：它的 `framework-arduinoespressif32-libs` 包里 `esp32s3/sdkconfig` 依旧是 `CONFIG_BT_CTRL_SLEEP_MODE_EFF=0`、`CONFIG_PM_ENABLE` 未开——所以这个环境的价值是「NimBLE 2.x / core 3.x 升级路线」，**不是省电**；
3. **本机（中文工作区路径 + Git Bash）编它有两个坑，已实测踩过**：
   - Git Bash 里编会挂：工具链后安装的 `idf_tools.py` 检测到 MSYSTEM 环境变量直接拒绝（`MSys/Mingw is not supported`；MSYS 会重新注入该变量，`env -u` 无效）。要么用 PowerShell/cmd，要么用 Python 启动器清环境：`python -c "import os,subprocess,sys; env=dict(os.environ); env.pop('MSYSTEM',None); sys.exit(subprocess.call([sys.executable,'-m','platformio']+sys.argv[1:], env=env))" run -e esp32s3_next`；
   - 新工具链（GCC 14.2 / binutils）在**含中文的项目路径**下最后链接会报 `cannot open map file ... firmware.map: No such file or directory`（主环境的老工具链无此问题）。解法：`subst X: "D:\硕士毕业课题\Code\ESP32"` 把项目映成纯 ASCII 盘符后在 X:\ 里编（取消映射：`subst X: /d`，重启也会自动消失）。

结论：**运行态压进毫安级的唯一路径 = 完整迁移到 ESP-IDF 框架重写**（`framework = espidf` + 自带 sdkconfig 打开 `CONFIG_BT_LE_SLEEP_ENABLE`/`CONFIG_PM_ENABLE`），工作量 = BLE 层 + ADC/RMT/NVS 全部按 IDF API 重写，需要时单独立项。

---

## 10. 验收清单（对应方案第 25 节）

- [ ] 上电状态必须 OFF
- [ ] BLE 连接稳定，控制延迟人眼无明显延迟
- [ ] 激光控制 100% 响应（Phase 1 先连续开关 100 次确认硬件稳定）
- [ ] BLE 掉线自动 OFF
- [ ] 电池供电下无人连接 5 min 自动关机（插着 USB 不触发，见 9.4）
- [ ] 状态反馈与实物一致
- [ ] 电压误差校准后 < ±0.05 V（**仅方案 B（GPIO1）可达标**；方案 A 用 GPIO3 时改判为“能区分 USB 供电 / 电池供电 / 低电量”）
- [ ] 电量显示满足状态判断即可
- [ ] 连续运行 2~4 h 无异常
- [ ] 断开后可重新连接
- [ ] 关机后电流明显下降（按 BOOT 关机，量到接近板上静态电流），按 RST 能正常开机
- [ ] 网页上改心跳灯 / 广播间隔后按 RST（或断电池再插上），重新连上还是改过的值
- [ ] USB 充电时固件运行不受影响
- [ ] 机械臂/电机工作时无线不掉线

---

## 11. 常见问题

**串口没有任何输出**
`platformio.ini` 已开 `ARDUINO_USB_CDC_ON_BOOT=1`，日志走原生 USB CDC；确认用的是板载 Type-C 口、波特率 115200、上电后重新打开监视器（启动日志只在最开始输出一次）。

**手机扫不到设备**
本工程**上限 1 台**：已经有一台连着的时候，固件会停止广播（省电），第二台设备这时扫不到——把第一台断开即可。此外还可能是：① 距离太远，或该客户端缓存了旧的 GAP 记录（把蓝牙关一下再开，或用 `--address` 直连）；② 广播间隔被改得太长（比如 10 s），手机要多等一会儿，网页上把「广播间隔」改回 500 ms 即可。

**开激光没反应、status 仍是 0**
① 电池读数低于 `BATTERY_CRITICAL_MV`（默认 3.40 V）时固件会拒绝开启，串口打印 `ON refused`，先充电或按实测调阈值；② 读数低于 `BATTERY_MIN_PLAUSIBLE_MV`（2.5 V）会被当作“ADC 未接”，此时**不**触发保护、激光照常能开——所以若串口既没有 `ON refused` 也没亮，问题在接线（激光 S 是否真的接到 GPIO4）而不是电量逻辑。

**电量一直显示 100%**
方案 A（GPIO3）下，**插着 USB 时这是正常的**——GPIO3 量的是 USB/VBUS 节点（≈4.9 V），对锂电池来说就是“满电/在充电”。拔掉 USB、只靠电池供电时才会显示真实电量。

**电量偏低、比万用表读数低 0.2~0.3 V**
方案 A 的固有现象：GPIO3 在肖特基二极管之后，读数天然少一个压降，且该压降随负载变化。用 7.2 第 3 步把 `BATTERY_DIODE_DROP_MV` 加大即可补偿；要彻底消除就切方案 B（GPIO1）。

**ADC 读数抖动大**
方案 A（GPIO3）是板载分压，抖动主要来自负载变化（激光开关、电机启停），属正常；方案 B（GPIO1）要确认 100 nF 已就近接在 GPIO1 与 GND 之间。两种情况都可以把 `BATTERY_SAMPLE_COUNT` 加到 32。

**想升级 NimBLE-Arduino 2.x**
回调签名变了（`onConnect(NimBLEServer*, NimBLEConnInfo&)`、`getValue()` 返回 `NimBLEAttValue`），需同步改 `src/ble_server.cpp`。第一版固定 1.4.3 不动。

**按 RST 没关机**
这是**故意的**：本工程按你的选择只用 BOOT 键关机，RST 就是纯粹的复位/开机（`POWER_RST_AS_SWITCH = 0`）。想要 RST 也能关机，把它改 `1`，再按 9.1 里那两个旋钮调复位原因的判断。

**关机后怎么开机 / 开不起来**
按 RST 或断电重上电都能开机（上电复位永远判成开机，不会卡在关机态）。真开不起来时，按住 BOOT 再点一下 RST 进下载模式，重新烧一次即可。

**按 BOOT 键没反应**
开机后 2 s 内不认（`POWER_BUTTON_IGNORE_MS`），而且要按满 60 ms（`POWER_BUTTON_DEBOUNCE_MS`）才算一次。

**网页上设的心跳灯 / 广播间隔，按 RST 之后还在吗**
在。两项都存在 NVS 里（`include/settings.h`，命名空间 `laser`），按 RST、断电、重新烧固件都保留；只有整片擦除（`pio run -t erase`）才会回到默认（心跳灯关、广播间隔 500 ms）。串口开机日志会打印读回来的值：`[indicator] heartbeat ON, 1000 ms on / 10000 ms period (from NVS)`、`[ble] adv interval = 500 ms (…x 0.625 ms)`。如果日志说 `[settings] NVS unavailable`，说明分区表里没有 nvs 分区，这时改动不会保存。

**关机后激光会不会还亮着**
不会。关机流程是「先 `laserOff()` → 把 GPIO4 用电平保持功能钉在 LOW → 灭灯 → 睡」，见 `src/power.cpp`。

---

## 12. 后续扩展（BLE 稳定后再做）

Phase 2 Wi-Fi：`POST /laser/on`、`POST /laser/off`、`GET /status`（JSON），见方案第 23 节。
再往后：OTA、空闲自动关机（多久没人连就自己睡）、Jetson 图形化界面。
