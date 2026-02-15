

# ExposonSense
**Edge-Intelligent Multi-Modal Data Acquisition Platform for Cycling Applications**
**边缘智能骑行多模态数据采集平台**

---

## 项目简介 | Project Overview

ExposonSense 是一套面向城市骑行与环境健康研究的边缘智能数据采集系统，包含两个互补的子平台：

ExposonSense is an edge-intelligent data acquisition system designed for urban cycling and environmental health research, comprising two complementary sub-platforms:

- **Exposon Sense**：多模态环境感知与路况采集系统（视觉+惯性+环境+定位）| Multi-modal environmental sensing and road condition acquisition system (Vision + IMU + Environmental + Positioning)
- **Exposon Air**：个人暴露组学监测平台（空气质量+惯性+定位）| Personal exposome monitoring platform (Air quality + IMU + Positioning)

两套系统均基于 ESP32-S3 架构构建，支持低功耗边缘计算与自适应电源管理，适用于移动场景下的长期环境监测、行为分析与健康暴露评估研究。

Both systems are built on the ESP32-S3 architecture, supporting low-power edge computing and adaptive power management strategies, suitable for long-term environmental monitoring, behavioral analysis, and health exposure assessment in mobile scenarios.

---

## 子项目一：Exposon Sense | Sub-project I: Exposon Sense

> **面向城市骑行场景的边缘计算数据采集系统**，集成视觉、惯性、环境及定位传感单元，实现骑行过程中多维度时空数据的同步采集与智能管理。该系统基于 ESP32-S3 架构构建，具备低功耗边缘计算能力与自适应电源管理策略，适用于移动场景下的长期环境监测与行为分析研究。

> **Edge computing data acquisition system for urban cycling scenarios**, integrating vision, inertial, environmental, and positioning sensing units to achieve synchronized multi-dimensional spatiotemporal data acquisition and intelligent management during cycling. Built on the ESP32-S3 architecture with low-power edge computing capabilities and adaptive power management strategies, suitable for long-term environmental monitoring and behavioral analysis in mobile contexts.

### 硬件架构 | Hardware Architecture

#### 模态传感阵列 | Multi-modal Sensor Array

| 模块 Module | 型号 Model | 关键参数 Key Specifications | 功能描述 Function Description |
|:---|:---|:---|:---|
| **视觉感知层 Vision Layer** | OV5640 | 5MP, 155°超广角，JPEG硬件编码 5MP, 155° ultra-wide angle, JPEG hardware encoding | 骑行前方全景路况采集，支持自动曝光与白平衡 Forward road condition panoramic capture with auto-exposure and white balance |
| **惯性测量单元 IMU** | MPU6050 | 六轴加速度计/陀螺仪，采样率1kHz，量程±16g/±2000°/s 6-axis accelerometer/gyroscope, 1kHz sampling, ±16g/±2000°/s range | 振动检测、姿态解算与运动状态识别 Vibration detection, attitude estimation, and motion state recognition |
| **环境感知层 Environmental Layer** | BMP280 + AHT20 | 气压精度±0.12hPa(等效±1m海拔)，温度±0.3°C，湿度±2%RH Pressure ±0.12hPa (±1m altitude equivalent), temp ±0.3°C, humidity ±2%RH | 微气候监测与坡度解算 Microclimate monitoring and gradient estimation |
| **定位授时层 Positioning Layer** | NEO-8M | GPS/GLONASS/北斗三模，10Hz更新，2.5m CEP，支持PPS授时 GPS/GLONASS/BeiDou tri-mode, 10Hz update, 2.5m CEP, PPS timing support | 高精度时空定位与数据对齐 High-precision spatiotemporal positioning and data synchronization |

#### 自适应功耗管理（三级状态机）| Adaptive Power Management (Three-State Machine)

| 模式 Mode | 触发条件 Trigger Condition | CPU频率 CPU Freq | 传感器状态 Sensor Status | 功耗特征 Power Profile |
|:---|:---|:---|:---|:---|
| **活跃模式 Active** | 移动检测 Motion detected | 240MHz | 全阵列工作，图像0.1Hz，数据记录30s间隔 Full array operation, image 0.1Hz, 30s data logging | 全功能运行 Full operation |
| **空闲模式 Idle** | 静止<5分钟 Stationary <5min | 160MHz | 维持GNSS热启动与振动监测，关闭成像 GNSS hot-start & vibration monitoring maintained, imaging off | 中度节能 Moderate saving |
| **深度休眠 Deep Sleep** | 静止>5分钟 Stationary >5min | 80MHz | 仅保留RTC与中断唤醒 RTC and interrupt wake-up only | <10mA |

### 边缘智能预处理 | Edge-Intelligent Preprocessing

- **冲击事件检测 Impact Event Detection**：75样本滑动窗口加速度方差分析（15秒历史均值），自动触发异常振动记录 75-sample sliding window acceleration variance analysis (15s historical mean), auto-triggering anomalous vibration logging
- **坡度实时解算 Real-time Gradient Estimation**：气压微分算法（ΔP/Δt，阈值±0.15hPa/15s），区分上坡/下坡/平路状态 Barometric differential algorithm (ΔP/Δt, threshold ±0.15hPa/15s), distinguishing uphill/downhill/flat states
- **时空数据对齐 Spatiotemporal Data Alignment**：GNSS授时UTC时间戳统一标记，支持10Hz高频定位更新 GNSS-timing UTC timestamp unification, supporting 10Hz high-frequency positioning updates

### 数据输出规范 | Data Output Specifications

| 数据类型 Data Type | 文件命名规则 File Naming Convention | 记录频率 Logging Frequency | 字段结构 Field Structure |
|:---|:---|:---|:---|
| 环境基线数据 Environmental Baseline | `YYYYMMDD_data.csv` | 30s-10min | UTC, 温度(°C), 湿度(%), 气压(hPa), 坡度状态, 纬度, 经度, 速度(km/h) UTC, Temp(°C), Humidity(%), Pressure(hPa), Gradient State, Lat, Lng, Speed(km/h) |
| 冲击事件日志 Impact Event Log | `YYYYMMDD_shock.csv` | 事件触发 Event-triggered | UTC, 加速度模值(m/s²), 15s滑动均值, 坡度状态, 位置坐标 UTC, Acceleration Magnitude(m/s²), 15s Moving Average, Gradient State, Coordinates |
| 图像数据 Image Data | `camera/YYYYMMDD_HHMMSS_[坡度状态].jpg` | 10s（移动中）10s (when moving) | EXIF嵌入GPS坐标与时间戳，155°广角畸变原始保留 EXIF-embedded GPS coordinates and timestamps, 155° wide-angle distortion preserved |
| 空闲监测数据 Idle Monitoring | `YYYYMMDD_data_idle.csv` | 10min | UTC, 状态标记, 温度, 湿度 UTC, State Flag, Temp, Humidity |

### 应用场景 | Application Scenarios

- 城市微气候移动监测 Urban microclimate mobile monitoring
- 道路基础设施健康评估 Road infrastructure health assessment
- 骑行行为与交通安全研究 Cycling behavior and traffic safety research
- 环境心理学与暴露科学 Environmental psychology and exposure science

---

## 子项目二：Exposon Air | Sub-project II: Exposon Air

> **适用于骑行的个人暴露组学监测平台**

> **Personal Exposome Monitoring Platform for Cycling Applications**

Exposon Air 是一款面向环境健康研究的便携式个人暴露监测（Personal Exposure Monitoring, PEM）系统，集成多参数空气质量传感、惯性测量与高精度时空定位功能，实现个体尺度环境暴露因素的连续追踪与记录。该系统基于 ESP32-S3 双核架构构建，采用双总线 I²C 拓扑与多路串口并行采集策略，支持 Wi-Fi 实时数据回传与本地冗余存储，适用于环境流行病学、职业健康与 urban mobility 领域的移动暴露评估研究。

Exposon Air is a portable Personal Exposure Monitoring (PEM) system designed for environmental health research, integrating multi-parameter air quality sensing, inertial measurement, and high-precision spatiotemporal positioning to enable continuous tracking of individual-scale environmental exposure factors. Built on the ESP32-S3 dual-core architecture with dual-bus I²C topology and multi-channel UART parallel acquisition strategy, supporting Wi-Fi real-time data transmission and local redundant storage, suitable for mobile exposure assessment research in environmental epidemiology, occupational health, and urban mobility domains.

### 硬件架构 | Hardware Architecture

| 模块 Module | 型号 Model | 参数规格 Specifications | 通信接口 Interface |
|:---|:---|:---|:---|
| **多合一环境传感器 Multi-sensor Environmental Module** | 集成模组 Integrated module | PM2.5/PM10/VOC/温湿度，激光散射原理，17字节协议帧，1Hz采样 PM2.5/PM10/VOC/Temp/RH, laser scattering, 17-byte protocol frame, 1Hz sampling | UART |
| **惯性测量单元 IMU** | MPU6050 | 三轴加速度/角速度/芯片温度，I²C 400kHz，量程±8g/±500°/s，21Hz数字低通滤波 3-axis accel/gyro/chip temp, I²C 400kHz, ±8g/±500°/s range, 21Hz digital LPF | I²C |
| **GNSS模块 GNSS Module** | NEO-8M | GPS/GLONASS/北斗，10Hz更新，2.5m CEP，输出经纬度/海拔/速度/UTC GPS/GLONASS/BeiDou, 10Hz update, 2.5m CEP, outputting lat/lng/altitude/speed/UTC | UART |

### 电源管理策略 | Power Management Strategy

- **GNSS可控电源 GNSS Controllable Power**：GPIO3控制VGNSS_CTRL，深度休眠时完全断电卫星模块 GPIO3-controlled VGNSS_CTRL, complete satellite module power-off during deep sleep
- **动态功耗调节 Dynamic Power Regulation**：Wi-Fi仅在数据上传时激活，空闲维持STA低功耗监听 Wi-Fi activated only during data upload, STA low-power listening during idle states

### 数据接口 | Data Interface

**本地存储（CSV结构化）| Local Storage (CSV Structured):**
```csv
Date,Time,PM2.5,PM10,VOC,Temp,Humidity,Ax,Ay,Az,Lat,Lng,Altitude,Satellites
2024-01-15,08:30:00,35,58,420,22.5,65.0,0.12,-0.05,9.81,31.2304,121.4737,15.2,8
```

