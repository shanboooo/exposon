#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <TinyGPS++.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>

// ================= 1. 系统配置中心 =================

#define TIMEZONE_OFFSET     8      // 时区设置 (北京时间 +8)
#define INTERVAL_DISPLAY    2000   // 屏幕刷新/切换页面的间隔 (ms)
#define INTERVAL_LOG        10000  // SD卡记录间隔 (ms)
#define SCREEN_TIMEOUT      60000  // 屏幕自动熄灭时间 (ms, 1分钟)

// --- 引脚定义 ---
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
#define VGNSS_CTRL  3  

#define AIR_RX      17 
#define AIR_TX      18 

#define SCREEN_ADDRESS 0x3C 

// ================= 2. 对象初始化 =================

TwoWire I2C_OLED = TwoWire(0);
TwoWire I2C_MPU  = TwoWire(1);

Adafruit_SSD1306 display(128, 32, &I2C_OLED, -1);
Adafruit_MPU6050 mpu;
TinyGPSPlus gps;
SPIClass *sdSPI = NULL;
HardwareSerial GPS_Serial(1);
HardwareSerial AirSerial(2);

// ================= 3. 数据结构 =================

struct {
  uint16_t voc = 0;
  uint16_t pm25 = 0;
  uint16_t pm10 = 0;
  float temp = 0.0;
  float humi = 0.0;
} airData;

struct {
  float ax = 0, ay = 0, az = 0;
} motion;

unsigned long lastActivityTime = 0;
bool isScreenOn = true;
bool isPageOne = true;

// ================= 4. 辅助工具函数 =================

String getLocalTimeString() {
  if (!gps.time.isValid()) return "00:00:00";
  int hour = gps.time.hour() + TIMEZONE_OFFSET;
  if (hour >= 24) hour -= 24;
  char buf[10];
  sprintf(buf, "%02d:%02d:%02d", hour, gps.time.minute(), gps.time.second());
  return String(buf);
}

String getLocalDateString() {
  if (!gps.date.isValid()) return "2026-01-01";
  char buf[15];
  sprintf(buf, "%04d-%02d-%02d", gps.date.year(), gps.date.month(), gps.date.day());
  return String(buf);
}

void handleAirSensor() {
  if (AirSerial.available() >= 17) {
    if (AirSerial.peek() != 0x3C) { AirSerial.read(); return; }
    uint8_t buf[17];
    AirSerial.readBytes(buf, 17);

    uint8_t checksum = 0;
    for (int i = 0; i < 16; i++) checksum += buf[i];

    if (checksum == buf[16]) {
      // 按照协议：B1-B2头，B3-B6是CO2/甲醛(跳过)，B7-B16是我们需要的数据
      airData.voc  = (buf[6] << 8) | buf[7];
      airData.pm25 = (buf[8] << 8) | buf[9];
      airData.pm10 = (buf[10] << 8) | buf[11];
      
      float t_int = (buf[12] & 0x7F);
      float t_dec = (float)buf[13] / 10.0;
      airData.temp = t_int + t_dec;
      if (buf[12] & 0x80) airData.temp = -airData.temp;
      airData.humi = (float)buf[14] + (float)buf[15] / 10.0;

      lastActivityTime = millis(); 
    }
  }
}

// ================= 5. SD卡每日日志逻辑 =================

void logToSD() {
  if (!gps.date.isValid()) return; // 没有日期暂不记录，防止文件名错误

  String fileName = "/" + getLocalDateString() + ".csv";
  
  // 检查文件是否存在，如果不存在则创建并写入表头
  if (!SD.exists(fileName)) {
    File file = SD.open(fileName, FILE_WRITE);
    if (file) {
      file.println("Date,Time,PM2.5(ug/m3),PM10(ug/m3),VOC(ug/m3),Temp(C),Humi(%),Ax,Ay,Az");
      file.close();
    }
  }

  // 追加数据
  File file = SD.open(fileName, FILE_APPEND);
  if (file) {
    file.print(getLocalDateString()); file.print(",");
    file.print(getLocalTimeString()); file.print(",");
    file.printf("%d,%d,%d,%.1f,%.1f,%.2f,%.2f,%.2f\n", 
                airData.pm25, airData.pm10, airData.voc, 
                airData.temp, airData.humi, 
                motion.ax, motion.ay, motion.az);
    file.close();
  }
}

// ================= 6. 核心逻辑 =================

void updateDisplay() {
  // 省电逻辑：超时关闭屏幕
  if (SCREEN_TIMEOUT > 0 && (millis() - lastActivityTime > SCREEN_TIMEOUT)) {
    if (isScreenOn) { display.ssd1306_command(SSD1306_DISPLAYOFF); isScreenOn = false; }
    return;
  } else {
    if (!isScreenOn) { display.ssd1306_command(SSD1306_DISPLAYON); isScreenOn = true; }
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("%s %s", gps.location.isValid() ? "GPS:OK" : "GPS:...", getLocalTimeString().c_str());
  display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

  display.setCursor(0, 12);
  if (isPageOne) {
    display.printf("PM2.5: %-4d VOC: %-4d", airData.pm25, airData.voc);
    display.setCursor(0, 22);
    display.printf("T:%.1fC  H:%.1f%%", airData.temp, airData.humi);
  } else {
    display.printf("PM10 : %-4d", airData.pm10);
    display.setCursor(0, 22);
    display.printf("Acc: %.1f, %.1f, %.1f", motion.ax, motion.ay, motion.az);
  }
  display.display();
  isPageOne = !isPageOne;
}

void setup() {
  Serial.begin(115200);
  I2C_OLED.begin(I2C0_SDA, I2C0_SCL, 400000); 
  I2C_MPU.begin(I2C1_SDA, I2C1_SCL, 400000); 

  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) Serial.println("OLED Error");
  if (!mpu.begin(0x68, &I2C_MPU)) Serial.println("MPU Error");

  sdSPI = new SPIClass(HSPI);
  sdSPI->begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if(!SD.begin(SD_CS, *sdSPI)) Serial.println("SD Error");

  GPS_Serial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX); 
  AirSerial.begin(9600, SERIAL_8N1, AIR_RX, AIR_TX);
  
  pinMode(VGNSS_CTRL, OUTPUT);
  digitalWrite(VGNSS_CTRL, HIGH);
  lastActivityTime = millis();
}

void loop() {
  while (GPS_Serial.available() > 0) gps.encode(GPS_Serial.read());
  handleAirSensor();

  static unsigned long lastDisp = 0;
  if (millis() - lastDisp >= INTERVAL_DISPLAY) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    motion.ax = a.acceleration.x; motion.ay = a.acceleration.y; motion.az = a.acceleration.z;
    updateDisplay();
    lastDisp = millis();
  }
  
  static unsigned long lastLog = 0;
  if (millis() - lastLog >= INTERVAL_LOG) {
    logToSD();
    lastLog = millis();
  }
}