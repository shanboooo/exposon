#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <Wire.h>
#include <TinyGPS++.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

// --- 传感器库冲突处理 ---
#define sensor_t adafruit_sensor_t
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Sensor.h>
#undef sensor_t 

// --- 引脚定义 (ESP32-S3 / ESP32-CAM 适配) ---
#define I2C_SDA 21
#define I2C_SCL 47
#define GPS_RX 1
#define GPS_TX 2
#define SD_CMD 38
#define SD_CLK 39
#define SD_D0 40

// --- 全局实例 ---
Adafruit_SSD1306 display(128, 32, &Wire, -1);
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;
Adafruit_MPU6050 mpu;
TinyGPSPlus gps;
HardwareSerial GPS_Serial(1);

// --- 全局变量与计时器 ---
char deviceID[13];
bool sdOK = false, camOK = false, timeSynced = false;
unsigned long last1sLog = 0, lastPhotoTime = 0, last5sSerial = 0, last1mEnv = 0;
int displayPage = 0;
unsigned long lastPageSwitch = 0;

// --- 融合逻辑相关变量 ---
float last_accel_mag = 0;
float motion_score = 0;    // 运动震动强度得分
#define MOTION_THRESHOLD 0.4  // 震动判定阈值 (根据实际抖动调整)
#define HDOP_OK 2.5           // 理想的卫星精度阈值

// --- 工具函数：获取 SD 卡剩余容量 ---
String getSDFreeGB() {
  if (!sdOK) return "ERR";
  uint64_t freeBytes = SD_MMC.totalBytes() - SD_MMC.usedBytes();
  return String((double)freeBytes / 1073741824.0, 1) + "G"; 
}

// --- MPU6050 运动检测逻辑 ---
bool isPhysicallyMoving() {
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);
  
  // 计算三轴加速度模长 (Vector Magnitude)
  float accel_mag = sqrt(sq(a.acceleration.x) + sq(a.acceleration.y) + sq(a.acceleration.z));
  
  // 计算震动增量 (当前与上一次的差异)
  float delta = abs(accel_mag - last_accel_mag);
  last_accel_mag = accel_mag;

  // 简单滤波，平滑震动得分
  motion_score = (motion_score * 0.8) + (delta * 0.2);
  
  return (motion_score > MOTION_THRESHOLD);
}

// --- 综合判定拍照逻辑 ---
bool shouldTakeSnapshot() {
  if (!camOK || !timeSynced) return false;

  bool hasPhysicalMotion = isPhysicallyMoving(); // MPU6050 检测到震动
  bool hasGPSSpeed = gps.speed.kmph() > 1.5;     // GPS 检测到移动速度
  bool hasGoodSignal = gps.hdop.hdop() < HDOP_OK; // 信号质量是否足以信任

  // 融合逻辑：必须有真实的物理震动，且 GPS 信号显示在移动或精度极高
  return (hasPhysicalMotion && (hasGPSSpeed || hasGoodSignal));
}

// --- 获取 UTC 时间字符串 ---
String getUTCNow() {
  struct tm info;
  if (!getLocalTime(&info) || info.tm_year < 126) return "WAITING..."; 
  char buf[20];
  strftime(buf, sizeof(buf), "%H:%M:%S", &info);
  return String(buf);
}

// --- 存储逻辑 ---
void logToSD(String type, String data) {
  if (!sdOK || !timeSynced) return; 
  struct tm info;
  if (!getLocalTime(&info)) return;

  char dir[15];
  strftime(dir, sizeof(dir), "/%Y%m%d", &info);
  if (!SD_MMC.exists(dir)) SD_MMC.mkdir(dir);

  char fileName[40];
  snprintf(fileName, sizeof(fileName), "%s/%s_%02d.CSV", dir, type.c_str(), info.tm_hour);
  
  File f = SD_MMC.open(fileName, FILE_APPEND);
  if (f) {
    // 自动在每条数据前加上标准 UTC 时间
    f.printf("%02d:%02d:%02d,%s\n", info.tm_hour, info.tm_min, info.tm_sec, data.c_str());
    f.close();
  }
}

// --- 拍照函数 (使用标准 UTC 时间) ---
void capturePhoto() {
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) return;

  struct tm info;
  getLocalTime(&info);
  
  char path[64];
  // 命名格式：/日期/IMG_时分秒_HDOP值.jpg (方便回溯信号质量)
  strftime(path, sizeof(path), "/%Y%m%d/IMG_%H%M%S", &info);
  String finalPath = String(path) + "_H" + String((int)gps.hdop.hdop()) + ".jpg";

  File f = SD_MMC.open(finalPath, FILE_WRITE);
  if (f) {
    f.write(fb->buf, fb->len);
    f.close();
    Serial.printf("[CAM] Saved: %s\n", finalPath.c_str());
  }
  esp_camera_fb_return(fb);
}

// --- OLED 显示更新 ---
void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0, 0);
  switch (displayPage) {
    case 0:
      display.printf("ID: %s\n", deviceID);
      display.printf("UTC: %s\n", getUTCNow().c_str());
      display.printf("GPS: %s\n", timeSynced ? "LOCKED" : "WAIT FIX...");
      display.printf("SD:%s  CAM:%s", getSDFreeGB().c_str(), camOK ? "OK" : "ERR");
      break;
    case 1: // GPS 增强页面
      display.printf("SAT: %d  HDOP: %.1f\n", gps.satellites.value(), gps.hdop.hdop());
      display.printf("SPD: %.1f km/h\n", gps.speed.kmph());
      display.printf("LAT: %.4f\n", gps.location.lat());
      display.printf("LNG: %.4f\n", gps.location.lng());
      break;
    case 2:
      sensors_event_t h_aht, t_aht; aht.getEvent(&h_aht, &t_aht);
      display.printf("TEMP: %.1f C\n", t_aht.temperature);
      display.printf("HUMI: %.1f %%\n", h_aht.relative_humidity);
      display.printf("PRES: %.1f hPa\n", bmp.readPressure() / 100.0);
      display.printf("MOTION: %.2f", motion_score);
      break;
  }
  display.display();
}

void setup() {
  Serial.begin(115200);
  GPS_Serial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Wire.begin(I2C_SDA, I2C_SCL);

  uint64_t mac = ESP.getEfuseMac();
  sprintf(deviceID, "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.display();

  aht.begin();
  if (!bmp.begin(0x77)) { if (!bmp.begin(0x76)) Serial.println("BMP280 Fail"); }
  mpu.begin();

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (SD_MMC.begin("/sdcard", true)) sdOK = true;

  camera_config_t cfg;
  cfg.pin_pwdn = -1; cfg.pin_reset = -1; cfg.pin_xclk = 15;
  cfg.pin_sccb_sda = 4; cfg.pin_sccb_scl = 5;
  cfg.pin_d7 = 16; cfg.pin_d6 = 17; cfg.pin_d5 = 18; cfg.pin_d4 = 12;
  cfg.pin_d3 = 10; cfg.pin_d2 = 8; cfg.pin_d1 = 9; cfg.pin_d0 = 11;
  cfg.pin_vsync = 6; cfg.pin_href = 7; cfg.pin_pclk = 13;
  cfg.xclk_freq_hz = 12000000;
  cfg.pixel_format = PIXFORMAT_JPEG; cfg.frame_size = FRAMESIZE_QSXGA;
  cfg.jpeg_quality = 12; cfg.fb_count = 1; cfg.fb_location = CAMERA_FB_IN_PSRAM;
  
  if (esp_camera_init(&cfg) == ESP_OK) camOK = true;

  struct timeval tv = { .tv_sec = 0 };
  settimeofday(&tv, NULL);
}

void loop() {
  while (GPS_Serial.available() > 0) gps.encode(GPS_Serial.read());

  // --- 时间同步 ---
  if (!timeSynced && gps.date.isValid() && gps.date.year() >= 2024) {
    struct tm t;
    t.tm_year = gps.date.year() - 1900;
    t.tm_mon = gps.date.month() - 1;
    t.tm_mday = gps.date.day();
    t.tm_hour = gps.time.hour();
    t.tm_min = gps.time.minute();
    t.tm_sec = gps.time.second();
    time_t now = mktime(&t);
    struct timeval tv = { .tv_sec = now };
    settimeofday(&tv, NULL);
    timeSynced = true; 
  }

  // --- [1秒定时任务] ---
  if (millis() - last1sLog >= 1000) {
    if (timeSynced && gps.location.isValid()) {
      // 核心修改：记录位置的同时，记录 HDOP(信号质量指标) 和 卫星数
      String gpsData = String(gps.location.lat(), 6) + "," + 
                       String(gps.location.lng(), 6) + "," + 
                       String(gps.speed.kmph(), 1) + "," +
                       String(gps.hdop.hdop(), 1) + "," + 
                       String(gps.satellites.value());
      logToSD("GPS", gpsData);
    }
    updateDisplay();
    last1sLog = millis();
  }

  // --- [动态拍照判断任务] ---
  // 根据运动强度决定是否拍照，且每 2.5 秒最多拍一张，避免 SD 卡写入阻塞
  if (shouldTakeSnapshot() && (millis() - lastPhotoTime >= 2500)) {
    capturePhoto();
    lastPhotoTime = millis();
  }

  // --- [1分钟环境记录] ---
  if (timeSynced && (millis() - last1mEnv >= 60000)) {
    sensors_event_t h_aht, t_aht; aht.getEvent(&h_aht, &t_aht);
    logToSD("ENV", String(t_aht.temperature, 1) + "," + String(h_aht.relative_humidity, 1));
    last1mEnv = millis();
  }

  // --- [5秒串口状态报告] ---
  if (millis() - last5sSerial >= 5000) {
    Serial.printf("[LOG] HDOP:%.1f | SAT:%d | Score:%.2f\n", 
                  gps.hdop.hdop(), gps.satellites.value(), motion_score);
    last5sSerial = millis();
  }

  // OLED 轮播
  if (millis() - lastPageSwitch > 4000) {
    displayPage = (displayPage + 1) % 3;
    lastPageSwitch = millis();
  }
}