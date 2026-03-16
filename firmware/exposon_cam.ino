#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <Wire.h>
#include <TinyGPS++.h>
#include <time.h>



//GPS 模块 TX → ESP32 GPIO 1 (代码中的 RX)
//GPS 模块 RX → ESP32 GPIO 2 (代码中的 TX)

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
bool sdOK = false, camOK = false, isMoving = false;
unsigned long last1sLog = 0, last2sPic = 0, last5sSerial = 0, last1mEnv = 0;
int displayPage = 0;
unsigned long lastPageSwitch = 0;

float ax_sum = 0, ay_sum = 0, az_sum = 0, pres_sum = 0;
int sample_count = 0;

// --- 工具函数：获取 UTC 时间字符串 ---
String getUTCNow() {
  struct tm info;
  if (!getLocalTime(&info)) return "00:00:00";
  char buf[20];
  // 修复点：添加 & 符号，将结构体转换为指针
  strftime(buf, sizeof(buf), "%H:%M:%S", &info); 
  return String(buf);
}

// --- 创建 README 说明文件 ---
void createReadme() {
  if (!sdOK) return;
  File f = SD_MMC.open("/README.TXT", FILE_WRITE);
  if (f) {
    f.println("=== SYSTEM INFORMATION ===");
    f.printf("Device ID: %s\n", deviceID);
    f.println("Generated at: " + getUTCNow());
    f.println("\n=== DATA FORMAT (DATA.CSV) ===");
    f.println("Format: [Time],[Type],[Data...]");
    f.println("1. ENV:  Temp(C), Humidity(%)");
    f.println("2. IMU:  Acc_X, Acc_Y, Acc_Z (m/s^2)");
    f.println("3. PRES: Barometric Pressure (hPa)");
    f.println("4. GPS:  Latitude, Longitude (Decimal)");
    f.close();
    Serial.println("README.TXT created.");
  }
}

// --- 结构化 SD 卡日志 ---
void logToSD(String type, String data) {
  if (!sdOK) return;
  struct tm info; getLocalTime(&info);
  char dir[15]; strftime(dir, sizeof(dir), "/%Y%m%d", &info);
  if (!SD_MMC.exists(dir)) SD_MMC.mkdir(dir);
  
  File f = SD_MMC.open(String(dir) + "/DATA.CSV", FILE_APPEND);
  if (f) {
    // 精简格式：时间,类型,数据
    f.printf("%s,%s,%s\n", getUTCNow().c_str(), type.c_str(), data.c_str());
    f.close();
  }
}

void setup() {
  Serial.begin(115200);
  GPS_Serial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Wire.begin(I2C_SDA, I2C_SCL);

  // 1. 生成唯一识别码
  uint64_t mac = ESP.getEfuseMac();
  sprintf(deviceID, "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);

  // 2. 初始化 OLED
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.display();

  // 3. 传感器初始化
  aht.begin();
  if (!bmp.begin(0x77)) { if (!bmp.begin(0x76)) Serial.println("BMP280 Fail"); }
  mpu.begin();

  // 4. SD 卡初始化
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (SD_MMC.begin("/sdcard", true)) {
    sdOK = true;
    createReadme(); // 自动生成 README.TXT
  }

  // 5. 摄像头初始化
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

  configTime(0, 0, "pool.ntp.org");
}

void loop() {
  while (GPS_Serial.available() > 0) gps.encode(GPS_Serial.read());

  isMoving = (gps.speed.kmph() > 2.0);

  if (isMoving) {
    sensors_event_t a, g, t_mpu;
    mpu.getEvent(&a, &g, &t_mpu);
    ax_sum += a.acceleration.x; ay_sum += a.acceleration.y; az_sum += a.acceleration.z;
    pres_sum += bmp.readPressure();
    sample_count++;
  }

  if (millis() - last1sLog >= 1000) {
    if (isMoving && sample_count > 0) {
      logToSD("IMU", String(ax_sum/sample_count) + "," + String(ay_sum/sample_count) + "," + String(az_sum/sample_count));
      logToSD("PRES", String((pres_sum/sample_count) / 100.0));
      logToSD("GPS", String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6));
      ax_sum = ay_sum = az_sum = pres_sum = 0; sample_count = 0;
    }
    
    if (gps.time.isUpdated()) {
      struct tm t;
      t.tm_year = gps.date.year() - 1900; t.tm_mon = gps.date.month() - 1; t.tm_mday = gps.date.day();
      t.tm_hour = gps.time.hour(); t.tm_min = gps.time.minute(); t.tm_sec = gps.time.second();
      time_t now = mktime(&t);
      struct timeval tv = { .tv_sec = now };
      settimeofday(&tv, NULL);
    }
    updateDisplay();
    last1sLog = millis();
  }

  if (isMoving && camOK && (millis() - last2sPic >= 2000)) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (fb) {
      struct tm info; getLocalTime(&info);
      char path[35]; strftime(path, sizeof(path), "/%Y%m%d/IMG_%H%M%S.jpg", &info);
      File f = SD_MMC.open(path, FILE_WRITE);
      if (f) { f.write(fb->buf, fb->len); f.close(); }
      esp_camera_fb_return(fb);
    }
    last2sPic = millis();
  }

  if (millis() - last5sSerial >= 5000) {
    sensors_event_t h_aht, t_aht; aht.getEvent(&h_aht, &t_aht);
    Serial.println("\n--- [SYSTEM REPORT] ---");
    Serial.printf("DeviceID: %s | Time: %s UTC\n", deviceID, getUTCNow().c_str());
    Serial.printf("STAT: Moving:%s, SD:%s, CAM:%s\n", isMoving ? "YES" : "NO", sdOK ? "OK" : "ERR", camOK ? "OK" : "ERR");
    Serial.println("------------------------");
    last5sSerial = millis();
  }

  if (millis() - last1mEnv >= 60000) {
    sensors_event_t h, t; aht.getEvent(&h, &t);
    logToSD("ENV", String(t.temperature) + "," + String(h.relative_humidity));
    last1mEnv = millis();
  }

  if (millis() - lastPageSwitch > 3000) {
    displayPage = (displayPage + 1) % 3;
    lastPageSwitch = millis();
  }
}

void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0, 0);
  switch (displayPage) {
    case 0:
      display.printf("UID: %s\n", deviceID);
      display.printf("UTC: %s\n", getUTCNow().c_str());
      display.printf("MOVING: %s\n", isMoving ? "YES" : "NO");
      display.printf("SD:%s  CAM:%s", sdOK ? "OK" : "ERR", camOK ? "OK" : "ERR");
      break;
    case 1:
      display.printf("SAT: %d SPD: %.1f\n", gps.satellites.value(), gps.speed.kmph());
      display.printf("LAT: %.5f\n", gps.location.lat());
      display.printf("LNG: %.5f\n", gps.location.lng());
      display.printf("ALT: %.1fm", gps.altitude.meters());
      break;
    case 2:
      sensors_event_t h, t; aht.getEvent(&h, &t);
      display.printf("TEMP: %.1f C\n", t.temperature);
      display.printf("HUMI: %.1f %%\n", h.relative_humidity);
      display.printf("PRES: %.1f hPa\n", bmp.readPressure() / 100.0);
      display.printf("V: 5MP-READY");
      break;
  }
  display.display();
}