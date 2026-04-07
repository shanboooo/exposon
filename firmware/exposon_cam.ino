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
float motion_score = 0;    
#define MOTION_THRESHOLD 0.4  // 震动灵敏度阈值
#define HDOP_OK 2.5           // GPS 精度阈值

// --- 获取 SD 卡剩余容量 ---
String getSDFreeGB() {
  if (!sdOK) return "ERR";
  uint64_t freeBytes = SD_MMC.totalBytes() - SD_MMC.usedBytes();
  return String((double)freeBytes / 1073741824.0, 1) + "G"; 
}

// --- 自动生成 README (供 Python 脚本识别 Device ID) ---
void writeReadme() {
  if (!sdOK) return;
  if (!SD_MMC.exists("/README.TXT")) {
    File f = SD_MMC.open("/README.TXT", FILE_WRITE);
    if (f) {
      f.println("==========================================");
      f.println("   ESP32-CAM TELEMETRY SYSTEM README      ");
      f.println("==========================================");
      f.printf("Device ID: %s\n", deviceID);
      f.println("Data Logic: Motion-Triggered (GPS/IMU/CAM)");
      f.close();
      Serial.println("[SD] README.TXT Created");
    }
  }
}

// --- 运动状态综合判定 ---
bool isVehicleMoving() {
  // 获取 MPU6050 数据计算震动强度
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);
  float accel_mag = sqrt(sq(a.acceleration.x) + sq(a.acceleration.y) + sq(a.acceleration.z));
  float delta = abs(accel_mag - last_accel_mag);
  last_accel_mag = accel_mag;
  motion_score = (motion_score * 0.8) + (delta * 0.2); // 平滑滤波

  // 判断条件：必须有时间同步，且检测到震动，且 GPS 信号显示在走动或精度极佳
  bool hasPhysicalMotion = (motion_score > MOTION_THRESHOLD);
  bool hasGPSSpeed = gps.speed.kmph() > 1.5;
  bool hasGoodSignal = gps.hdop.hdop() < HDOP_OK;

  return (timeSynced && hasPhysicalMotion && (hasGPSSpeed || hasGoodSignal));
}

// --- CSV 存储逻辑 ---
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
    f.printf("%02d:%02d:%02d,%s\n", info.tm_hour, info.tm_min, info.tm_sec, data.c_str());
    f.close();
  }
}

// --- 拍照逻辑 ---
void capturePhoto() {
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) return;
  struct tm info;
  getLocalTime(&info);
  char path[64];
  strftime(path, sizeof(path), "/%Y%m%d/IMG_%H%M%S", &info);
  String finalPath = String(path) + "_H" + String((int)gps.hdop.hdop()) + ".jpg";
  File f = SD_MMC.open(finalPath, FILE_WRITE);
  if (f) {
    f.write(fb->buf, fb->len);
    f.close();
    Serial.printf("[CAM] Captured: %s\n", finalPath.c_str());
  }
  esp_camera_fb_return(fb);
}

// --- OLED 界面渲染 ---
void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0, 0);
  switch (displayPage) {
    case 0:
      display.printf("ID: %s\n", deviceID);
      display.printf("GPS: %s\n", timeSynced ? "LOCKED" : "WAIT...");
      display.printf("SD:%s  CAM:%s", getSDFreeGB().c_str(), camOK ? "OK" : "ERR");
      break;
    case 1: 
      display.printf("SAT: %d  HDOP: %.1f\n", gps.satellites.value(), gps.hdop.hdop());
      display.printf("SPD: %.1f km/h\n", gps.speed.kmph());
      display.printf("LAT: %.4f\n", gps.location.lat());
      break;
    case 2:
      sensors_event_t h, t; aht.getEvent(&h, &t);
      display.printf("TEMP: %.1f C\n", t.temperature);
      display.printf("HUMI: %.1f %%\n", h.relative_humidity);
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
  if (!bmp.begin(0x77)) { if (!bmp.begin(0x76)) Serial.println("BMP Fail"); }
  mpu.begin();

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (SD_MMC.begin("/sdcard", true)) {
    sdOK = true;
    writeReadme();
  }

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

  // --- GPS 时间同步逻辑 ---
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

  // --- 运动判定状态更新 ---
  bool isMoving = isVehicleMoving();

  // --- [1秒定时任务]：记录 GPS 和 IMU ---
  if (millis() - last1sLog >= 1000) {
    if (isMoving) {
      // 记录 GPS (Lat, Lng, Speed, HDOP, Sats)
      if (gps.location.isValid()) {
        logToSD("GPS", String(gps.location.lat(), 6) + "," + 
                       String(gps.location.lng(), 6) + "," + 
                       String(gps.speed.kmph(), 1) + "," +
                       String(gps.hdop.hdop(), 1) + "," + 
                       String(gps.satellites.value()));
      }
      // 记录 IMU (ax, ay, az)
      sensors_event_t a, g, t;
      mpu.getEvent(&a, &g, &t);
      logToSD("IMU", String(a.acceleration.x, 2) + "," + 
                     String(a.acceleration.y, 2) + "," + 
                     String(a.acceleration.z, 2));
    }
    updateDisplay();
    last1sLog = millis();
  }

  // --- [动态拍照判断] ---
  if (camOK && isMoving && (millis() - lastPhotoTime >= 2500)) {
    capturePhoto();
    lastPhotoTime = millis();
  }

  // --- [1分钟环境记录]：环境数据定时记录，不限运动 ---
  if (timeSynced && (millis() - last1mEnv >= 60000)) {
    sensors_event_t h, t; aht.getEvent(&h, &t);
    logToSD("ENV", String(t.temperature, 1) + "," + String(h.relative_humidity, 1));
    last1mEnv = millis();
  }

  // OLED 页面自动轮播 (8秒)
  if (millis() - lastPageSwitch > 8000) {
    displayPage = (displayPage + 1) % 3;
    lastPageSwitch = millis();
  }
}