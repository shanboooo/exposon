#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <Wire.h>
#include <TinyGPS++.h>
#include <time.h>
#include <sys/time.h>

// --- 传感器库冲突处理 ---
#define sensor_t adafruit_sensor_t
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Sensor.h>
#undef sensor_t 

// --- 引脚定义 ---
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
bool sdOK = false, camOK = false, isMoving = false, timeSynced = false;
unsigned long last1sLog = 0, last2sPic = 0, last5sSerial = 0, last1mEnv = 0;
int displayPage = 0;
unsigned long lastPageSwitch = 0;

float ax_sum = 0, ay_sum = 0, az_sum = 0, pres_sum = 0;
int sample_count = 0;

// --- 工具函数：GPS 时间有效性检查 ---
bool isDateValid() {
  // 只有年份 >= 2026 才认为 GPS 已经获取到可靠日期（防止 1970/2002 错误日期文件夹）
  return (gps.date.isValid() && gps.date.year() >= 2026);
}

String getUTCNow() {
  struct tm info;
  // 如果年份还没到 2026，说明系统时钟还没同步
  if (!getLocalTime(&info) || info.tm_year < 126) return "WAITING..."; 
  char buf[20];
  strftime(buf, sizeof(buf), "%H:%M:%S", &info);
  return String(buf);
}

// --- 分文件、分小时、分类型存储逻辑 ---
void logToSD(String type, String data) {
  if (!sdOK || !timeSynced) return; 

  struct tm info;
  if (!getLocalTime(&info)) return;

  // 创建日期文件夹: /YYYYMMDD
  char dir[15];
  strftime(dir, sizeof(dir), "/%Y%m%d", &info);
  if (!SD_MMC.exists(dir)) SD_MMC.mkdir(dir);

  // 构造文件名: /YYYYMMDD/TYPE_HH.CSV
  char fileName[40];
  snprintf(fileName, sizeof(fileName), "%s/%s_%02d.CSV", dir, type.c_str(), info.tm_hour);
  
  File f = SD_MMC.open(fileName, FILE_APPEND);
  if (f) {
    f.printf("%02d:%02d:%02d,%s\n", info.tm_hour, info.tm_min, info.tm_sec, data.c_str());
    f.close();
  }
}

void createReadme() {
  if (!sdOK) return;
  File f = SD_MMC.open("/README.TXT", FILE_WRITE);
  if (f) {
    f.println("=== ESP32-CAM LOG SYSTEM ===");
    f.printf("Device ID: %s\n", deviceID);
    f.println("Mode: Separate Files per Hour/Type");
    f.close();
  }
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

  // 1. 初始化 SD 卡
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (SD_MMC.begin("/sdcard", true)) {
    sdOK = true;
    createReadme();
  } else {
    Serial.println("SD Card Mount Failed!");
  }

  // 2. 初始化摄像头
  camera_config_t cfg;
  cfg.pin_pwdn = -1; cfg.pin_reset = -1; cfg.pin_xclk = 15;
  cfg.pin_sccb_sda = 4; cfg.pin_sccb_scl = 5;
  cfg.pin_d7 = 16; cfg.pin_d6 = 17; cfg.pin_d5 = 18; cfg.pin_d4 = 12;
  cfg.pin_d3 = 10; cfg.pin_d2 = 8; cfg.pin_d1 = 9; cfg.pin_d0 = 11;
  cfg.pin_vsync = 6; cfg.pin_href = 7; cfg.pin_pclk = 13;
  cfg.xclk_freq_hz = 12000000;
  cfg.pixel_format = PIXFORMAT_JPEG; cfg.frame_size = FRAMESIZE_QSXGA;
  cfg.jpeg_quality = 12; cfg.fb_count = 1; cfg.fb_location = CAMERA_FB_IN_PSRAM;
  
  if (esp_camera_init(&cfg) == ESP_OK) {
    camOK = true;
  } else {
    Serial.println("Camera Init Failed!");
  }

  // 初始将系统时间归零（强制进入 WAITING 状态）
  struct timeval tv = { .tv_sec = 0 };
  settimeofday(&tv, NULL);
}

void loop() {
  while (GPS_Serial.available() > 0) gps.encode(GPS_Serial.read());

  // --- 时间同步逻辑 ---
  if (!timeSynced && isDateValid()) {
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
    Serial.println("\n[SYSTEM] >>> Time Synced via GPS. Recording UNLOCKED. <<<");
  }

  // --- 运动检测 ---
  isMoving = (gps.speed.kmph() > 1.5);

  if (isMoving) {
    sensors_event_t a, g, t_mpu;
    mpu.getEvent(&a, &g, &t_mpu);
    ax_sum += a.acceleration.x; ay_sum += a.acceleration.y; az_sum += a.acceleration.z;
    pres_sum += bmp.readPressure();
    sample_count++;
  }

  // --- [1秒任务] 记录运动数据 (IMU/GPS/PRES) ---
  if (millis() - last1sLog >= 1000) {
    if (isMoving && sample_count > 0 && timeSynced) {
      logToSD("IMU", String(ax_sum/sample_count) + "," + String(ay_sum/sample_count) + "," + String(az_sum/sample_count));
      logToSD("PRE", String((pres_sum/sample_count) / 100.0));
      logToSD("GPS", String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6) + "," + String(gps.speed.kmph(), 1));
      
      ax_sum = ay_sum = az_sum = pres_sum = 0; sample_count = 0;
    }
    updateDisplay();
    last1sLog = millis();
  }

  // --- [2秒任务] 拍照 ---
  if (isMoving && camOK && timeSynced && (millis() - last2sPic >= 2000)) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (fb) {
      struct tm info; getLocalTime(&info);
      char path[45]; strftime(path, sizeof(path), "/%Y%m%d/IMG_%H%M%S.jpg", &info);
      File f = SD_MMC.open(path, FILE_WRITE);
      if (f) { f.write(fb->buf, fb->len); f.close(); }
      esp_camera_fb_return(fb);
    }
    last2sPic = millis();
  }

  // --- [1分钟任务] 环境数据 (ENV) ---
  if (timeSynced && (millis() - last1mEnv >= 60000)) {
    sensors_event_t h_aht, t_aht; aht.getEvent(&h_aht, &t_aht);
    logToSD("ENV", String(t_aht.temperature, 1) + "," + String(h_aht.relative_humidity, 1));
    last1mEnv = millis();
  }

  // --- [5秒任务] 串口综合报告 (核心改动在这里) ---
  if (millis() - last5sSerial >= 5000) {
    Serial.printf("[SYSTEM] Time:%s | GPS:%s | SD:%s | CAM:%s\n", 
                  getUTCNow().c_str(), 
                  timeSynced ? "SYNCED" : "SEARCHING...", 
                  sdOK ? "OK" : "ERR", 
                  camOK ? "OK" : "ERR"); // 增加了摄像头状态显示
    last5sSerial = millis();
  }

  // OLED 页面轮播
  if (millis() - lastPageSwitch > 4000) {
    displayPage = (displayPage + 1) % 3;
    lastPageSwitch = millis();
  }
}

void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0, 0);
  switch (displayPage) {
    case 0:
      display.printf("ID: %s\n", deviceID);
      display.printf("UTC: %s\n", getUTCNow().c_str());
      display.printf("GPS: %s\n", timeSynced ? "LOCKED" : "WAIT FIX...");
      display.printf("SD:%s  CAM:%s", sdOK ? "OK" : "ERR", camOK ? "OK" : "ERR");
      break;
    case 1:
      display.printf("SAT: %d  SPD:%.1f\n", gps.satellites.value(), gps.speed.kmph());
      display.printf("LAT: %.4f\n", gps.location.lat());
      display.printf("LNG: %.4f\n", gps.location.lng());
      display.printf("ALT: %.0fm", gps.altitude.meters());
      break;
    case 2:
      sensors_event_t h_aht, t_aht; aht.getEvent(&h_aht, &t_aht);
      display.printf("TEMP: %.1f C\n", t_aht.temperature);
      display.printf("HUMI: %.1f %%\n", h_aht.relative_humidity);
      display.printf("PRES: %.1f hPa\n", bmp.readPressure() / 100.0);
      display.print("REC: SEPARATE MODE");
      break;
  }
  display.display();
}