#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <TinyGPS++.h>
#include <SPI.h>
#include "SdFat.h"       // 必须安装 SdFat 库
#include <Wire.h> 
#include <time.h>
#include <sys/time.h>

// ================= 1. 引脚定义 =================
#define I2C0_SDA    15 
#define I2C0_SCL    16
#define I2C1_SDA    42
#define I2C1_SCL    41
#define SD_SCK      6
#define SD_MISO     5
#define SD_MOSI     7
#define SD_CS       4
#define GPS_RX      38
#define GPS_TX      39
#define AIR_RX      17
#define AIR_TX      18 
#define VGNSS_CTRL  3  

// OLED 参数
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET    -1

// ================= 2. 对象初始化 =================
TwoWire I2C_OLED = TwoWire(0);
TwoWire I2C_MPU  = TwoWire(1);

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &I2C_OLED, OLED_RESET);
Adafruit_MPU6050 mpu;
TinyGPSPlus gps;

// SdFat 核心对象：SdFs 自动兼容 FAT16/FAT32/exFAT
SdFs sd;
FsFile f_log;
SPIClass sdSPI(HSPI);

HardwareSerial GPS_Serial(1);
HardwareSerial AirSerial(2);

// ================= 3. 全局变量 =================
struct {
  uint16_t pm25 = 0, pm10 = 0, voc = 0;
  float temp = 0.0, humi = 0.0;
} airData;

// 用于 1 分钟加权计算的累加器
struct {
  uint32_t pm25Sum = 0, pm10Sum = 0, vocSum = 0;
  float tempSum = 0.0, humiSum = 0.0;
  uint32_t sampleCount = 0;
} airAggr;

struct {
  float ax = 0, ay = 0, az = 0;
} motionData;

char deviceID[13];
bool sdOK = false, isMoving = false, timeSynced = false;
int displayPage = 0;
float sdFreeGB = 0.0; 

unsigned long last1sTask = 0;
unsigned long last1mTask = 0; 
unsigned long lastTimeSync = 0;
unsigned long lastPageSwitch = 0;

// ================= 4. 辅助与存储函数 =================

// 修正后的容量计算函数
void updateSDSpace() {
  if (!sdOK) return;
  uint32_t freeClusters = sd.freeClusterCount();
  if (freeClusters != (uint32_t)-1) {
    uint64_t freeBytes = (uint64_t)freeClusters * sd.sectorsPerCluster() * 512;
    sdFreeGB = (float)freeBytes / (1024.0 * 1024.0 * 1024.0);
  }
}

String getUTCNow() {
  struct tm info;
  if (!getLocalTime(&info) || info.tm_year < 120) return "WAIT GPS..."; 
  char buf[20];
  strftime(buf, sizeof(buf), "%H:%M:%S", &info);
  return String(buf);
}

void syncTimeViaGPS() {
  if (gps.date.isValid() && gps.date.year() >= 2024 && gps.time.isValid()) {
    struct tm t = {0};
    t.tm_year = gps.date.year() - 1900;
    t.tm_mon  = gps.date.month() - 1;
    t.tm_mday = gps.date.day();
    t.tm_hour = gps.time.hour();
    t.tm_min  = gps.time.minute();
    t.tm_sec  = gps.time.second();
    time_t now = mktime(&t);
    struct timeval tv = { .tv_sec = now };
    settimeofday(&tv, NULL);
    timeSynced = true;
    lastTimeSync = millis();
  }
}

void logToSD(String type, String data) {
  if (!sdOK || !timeSynced) return; 
  struct tm info;
  if (!getLocalTime(&info)) return;

  char dir[15];
  strftime(dir, sizeof(dir), "/%Y%m%d", &info);
  if (!sd.exists(dir)) sd.mkdir(dir);

  char fileName[40];
  snprintf(fileName, sizeof(fileName), "%s/%s_%02d.CSV", dir, type.c_str(), info.tm_hour);
  
  if (f_log.open(fileName, O_RDWR | O_CREAT | O_AT_END)) {
    char tStr[15];
    strftime(tStr, sizeof(tStr), "%H:%M:%S", &info);
    f_log.printf("%s,%s\n", tStr, data.c_str());
    f_log.close();
  }
}

// === 新增：在 SD 卡生成 README 描述文件 ===
void createReadmeOnSD() {
  if (!sdOK) return;
  FsFile f_readme;
  // 使用截断写入模式，每次开机都会更新这个文件，确保 ID 是正确的
  if (f_readme.open("/README.txt", O_WRONLY | O_CREAT | O_TRUNC)) {
    f_readme.println("========================================");
    f_readme.println("       Environment & Motion Logger      ");
    f_readme.println("========================================");
    f_readme.printf("Device ID (MAC): %s\n\n", deviceID);
    
    f_readme.println("--- Data Formats ---");
    f_readme.println("Logs are saved in /YYYYMMDD/ folders.");
    f_readme.println("Every line starts with the UTC Time (HH:MM:SS).\n");
    
    f_readme.println("[1] GPS Data (GPS_HH.CSV)");
    f_readme.println("Format: Time, Latitude, Longitude, Speed(km/h), HDOP, Satellites");
    
    f_readme.println("\n[2] IMU Data (IMU_HH.CSV)");
    f_readme.println("Format: Time, Accel_X(g), Accel_Y(g), Accel_Z(g)");
    
    f_readme.println("\n[3] Air Quality Data (AIR_HH.CSV)");
    f_readme.println("Format: Time, PM2.5, PM10, VOC, Temperature(C), Humidity(%)");
    
    f_readme.println("========================================");
    f_readme.close();
    Serial.println("README.txt generated on SD Card.");
  }
}

// ================= 5. 传感器读取 =================

void readMPU() {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  motionData.ax = a.acceleration.x;
  motionData.ay = a.acceleration.y;
  motionData.az = a.acceleration.z;
}

void handleAirSensor() {
  while (AirSerial.available() >= 17) {
    if (AirSerial.peek() != 0x3C) { AirSerial.read(); continue; }
    uint8_t buf[17];
    AirSerial.readBytes(buf, 17);
    uint8_t sum = 0;
    for (int i = 0; i < 16; i++) sum += buf[i];
    
    if (sum == buf[16]) {
      airData.voc  = (buf[6] << 8) | buf[7];
      airData.pm25 = (buf[8] << 8) | buf[9];
      airData.pm10 = (buf[10] << 8) | buf[11];
      float t_val = (buf[12] & 0x7F) + (float)buf[13] / 10.0;
      if (buf[12] & 0x80) t_val = -t_val;
      airData.temp = t_val;
      airData.humi = buf[14] + (float)buf[15] / 10.0;

      airAggr.pm25Sum += airData.pm25;
      airAggr.pm10Sum += airData.pm10;
      airAggr.vocSum  += airData.voc;
      airAggr.tempSum += airData.temp;
      airAggr.humiSum += airData.humi;
      airAggr.sampleCount++;
    }
  }
}

// ================= 6. 界面显示 =================

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  switch (displayPage) {
    case 0: { 
      display.printf("ID:%s\n", deviceID);
      display.printf("UTC:%s\n", getUTCNow().c_str());
      if (sdOK) display.printf("Free:%.2f GB\n", sdFreeGB);
      else display.println("SD: ERROR");
      display.printf("Mode:%s", isMoving ? "MOVING" : "STATIC");
      break;
    }
    case 1: { 
      display.printf("Sats:%d HDOP:%.1f\n", gps.satellites.value(), gps.hdop.hdop());
      display.printf("Spd:%.1f km/h\n", gps.speed.kmph());
      display.printf("Lat:%.4f\n", gps.location.lat());
      display.printf("Lng:%.4f\n", gps.location.lng());
      break;
    }
    case 2: { 
      display.printf("PM2.5:%d  PM10:%d\n", airData.pm25, airData.pm10);
      display.printf("VOC:%d\n", airData.voc);
      display.printf("T:%.1fC  H:%.1f%%\n", airData.temp, airData.humi);
      break;
    }
  }
  display.display();
}

// ================= 7. Setup & Loop =================

void setup() {
  Serial.begin(115200);
  
  // 优化：采用完整 MAC 生成 12 位唯一序列号，减少撞号几率
  uint64_t mac = ESP.getEfuseMac();
  sprintf(deviceID, "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);

  I2C_OLED.begin(I2C0_SDA, I2C0_SCL, 400000); 
  I2C_MPU.begin(I2C1_SDA, I2C1_SCL, 400000); 

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay(); display.display();

  if (mpu.begin(0x68, &I2C_MPU)) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  }

  // 初始化 SPI 和 SdFat
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (sd.begin(SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(16), &sdSPI))) {
    sdOK = true;
    updateSDSpace(); 
    createReadmeOnSD(); // 增加：创建并写入 README.txt
  } else {
    Serial.println("SD Card Error (exFAT?)");
  }

  GPS_Serial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  AirSerial.begin(9600, SERIAL_8N1, AIR_RX, AIR_TX);
  
  pinMode(VGNSS_CTRL, OUTPUT);
  digitalWrite(VGNSS_CTRL, HIGH);
  
  struct timeval tv = { .tv_sec = 0 };
  settimeofday(&tv, NULL);
}

void loop() {
  while (GPS_Serial.available() > 0) gps.encode(GPS_Serial.read());
  handleAirSensor();

  if (!timeSynced || (millis() - lastTimeSync >= 600000)) {
    syncTimeViaGPS();
  }

  isMoving = (gps.speed.kmph() > 1.5);

  // 1 秒高频任务
  if (millis() - last1sTask >= 1000) {
    if (isMoving && timeSynced) {
      readMPU();
      String imuStr = String(motionData.ax) + "," + String(motionData.ay) + "," + String(motionData.az);
      
      // 增加：GPS 字符串内包含 HDOP 和 卫星数
      String gpsStr = String(gps.location.lat(), 6) + "," + 
                      String(gps.location.lng(), 6) + "," + 
                      String(gps.speed.kmph(), 1) + "," +
                      String(gps.hdop.hdop(), 1) + "," + 
                      String(gps.satellites.value());
                      
      logToSD("IMU", imuStr);
      logToSD("GPS", gpsStr);
    }
    last1sTask = millis();
  }

  // 1 分钟加权存储任务
  if (millis() - last1mTask >= 60000) {
    if (timeSynced && airAggr.sampleCount > 0) {
      float avgPM25 = (float)airAggr.pm25Sum / airAggr.sampleCount;
      float avgPM10 = (float)airAggr.pm10Sum / airAggr.sampleCount;
      float avgVOC  = (float)airAggr.vocSum / airAggr.sampleCount;
      float avgTemp = airAggr.tempSum / airAggr.sampleCount;
      float avgHumi = airAggr.humiSum / airAggr.sampleCount;

      String airStr = String(avgPM25, 1) + "," + String(avgPM10, 1) + "," + 
                      String(avgVOC, 1) + "," + String(avgTemp, 1) + "," + String(avgHumi, 1);
      logToSD("AIR", airStr);

      airAggr.pm25Sum = 0; airAggr.pm10Sum = 0; airAggr.vocSum = 0;
      airAggr.tempSum = 0; airAggr.humiSum = 0;
      airAggr.sampleCount = 0;
      
      updateSDSpace();
    }
    last1mTask = millis();
  }

  // 4 秒 UI 轮播
  if (millis() - lastPageSwitch >= 4000) {
    updateDisplay();
    displayPage = (displayPage + 1) % 3;
    lastPageSwitch = millis();
  }
}