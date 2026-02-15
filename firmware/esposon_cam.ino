#include "esp_camera.h"
#include <time.h> 
#include <sys/time.h>
#include "FS.h"
#include "SD_MMC.h"
#include <Wire.h>
#include <TinyGPSPlus.h>

// --- 用户可调参数配置区 ---
#define PHOTO_INTERVAL 5000       // 拍照间隔 (5秒)
#define IDLE_LOG_INTERVAL 600000  // 静止状态记录间隔 (10分钟)
#define MOVE_LOG_INTERVAL 5000    // 移动时数据记录间隔 (5秒)
#define POWER_SAVE_DELAY 300000   // 进入省电模式等待 (5分钟)

// --- 冲突修复与传感器库 ---
#define sensor_t adafruit_sensor_t
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Sensor.h>
#undef sensor_t 

// --- 引脚定义 (ESP32-S3 Cam 常用引脚) ---
#define I2C_SDA 21
#define I2C_SCL 47
#define GPS_RX 1
#define GPS_TX 2
#define SD_CMD 38
#define SD_CLK 39
#define SD_D0 40

// --- 全局变量 ---
Adafruit_SSD1306 display(128, 32, &Wire, -1);
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;
Adafruit_MPU6050 mpu;
TinyGPSPlus gps;
HardwareSerial GPS_Serial(1);

unsigned long lastPhotoTime = 0, lastLogTime = 0;
unsigned long lastSlopeCheck = 0, lastTimeSave = 0, lastShockRecord = 0;
unsigned long lastMoveTime = 0, photoStatusTimer = 0;
bool isMoving = false, timeSynced = false, isPowerSaving = false, showPhotoStatus = false;
float lastPressure = 0, sdFreeGB = 0;
String slopeStatus = "Flat";

// ================================================================
// --- 工具函数：时间与文件管理 ---
// ================================================================

// 获取格式化时间字符串
String getUTCString(bool filenameMode = false) {
    time_t now; struct tm ti; time(&now); gmtime_r(&now, &ti);
    char buf[32];
    if (filenameMode) strftime(buf, sizeof(buf), "%H%M%S", &ti);
    else strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
    return String(buf);
}

// 获取每日命名的文件名 (例如 /20260215_data.csv)
String getDailyFileName(String suffix) {
    time_t now; struct tm ti; time(&now); gmtime_r(&now, &ti);
    char buf[64];
    strftime(buf, sizeof(buf), "/%Y%m%d_", &ti);
    return String(buf) + suffix;
}

void updateSDSpace() {
    uint64_t bytesFree = SD_MMC.totalBytes() - SD_MMC.usedBytes();
    sdFreeGB = (float)bytesFree / 1024.0 / 1024.0 / 1024.0;
}

// ================================================================
// --- 核心功能：相机与传感器 ---
// ================================================================

void setupCamera() {
    camera_config_t cfg;
    cfg.pin_pwdn = -1; cfg.pin_reset = -1; cfg.pin_xclk = 15;
    cfg.pin_sccb_sda = 4; cfg.pin_sccb_scl = 5;
    cfg.pin_d7 = 16; cfg.pin_d6 = 17; cfg.pin_d5 = 18; cfg.pin_d4 = 12;
    cfg.pin_d3 = 10; cfg.pin_d2 = 8; cfg.pin_d1 = 9; cfg.pin_d0 = 11;
    cfg.pin_vsync = 6; cfg.pin_href = 7; cfg.pin_pclk = 13;
    cfg.xclk_freq_hz = 20000000; cfg.ledc_timer = LEDC_TIMER_0; cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.pixel_format = PIXFORMAT_JPEG; cfg.frame_size = FRAMESIZE_UXGA; 
    cfg.jpeg_quality = 10; cfg.fb_count = 2; cfg.grab_mode = CAMERA_GRAB_LATEST;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
    
    if (esp_camera_init(&cfg) != ESP_OK) return;

    sensor_t * s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 1); s->set_contrast(s, 1); s->set_saturation(s, 1);
        s->set_whitebal(s, 1); s->set_awb_gain(s, 1); s->set_aec2(s, 1);
    }
}

void capturePhoto() {
    if (isPowerSaving) return; 
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return;
    
    // 照片存放在 /camera 目录下，带日期和时间
    String path = "/camera/" + getDailyFileName(getUTCString(true) + "_" + slopeStatus + ".jpg");
    File file = SD_MMC.open(path, FILE_WRITE);
    if (file) { 
        file.write(fb->buf, fb->len); 
        file.close(); 
        showPhotoStatus = true;
        photoStatusTimer = millis();
    }
    esp_camera_fb_return(fb);
    lastPhotoTime = millis();
}

void checkShock() {
    sensors_event_t a, g, t;
    if(!mpu.getEvent(&a, &g, &t)) return;
    static float history[75]; static int idx = 0; static bool bufferFull = false;
    float currentTotal = sqrt(sq(a.acceleration.x) + sq(a.acceleration.y) + sq(a.acceleration.z));
    
    float runningAvg = 9.81;
    int count = bufferFull ? 75 : idx;
    if (count > 0) {
        float sum = 0;
        for(int i=0; i<count; i++) sum += history[i];
        runningAvg = sum / (float)count;
    }
    
    // 判定冲击：瞬时值超过平均值2倍且绝对差值较大
    if (currentTotal > (runningAvg * 2.0) && abs(currentTotal - runningAvg) > 12.0) {
        if (millis() - lastShockRecord > 1000) {
            String fileName = getDailyFileName("shock.csv");
            File f = SD_MMC.open(fileName, FILE_APPEND);
            if (f) {
                if (f.size() == 0) f.println("UTC,G_Force,Avg_G,Slope,Lat,Lng"); // 写入表头
                f.printf("%s,%.2f,%.2f,%s,%.6f,%.6f\n", getUTCString().c_str(), currentTotal, runningAvg, slopeStatus.c_str(), gps.location.lat(), gps.location.lng());
                f.close();
            }
            lastShockRecord = millis();
        }
    }
    history[idx] = currentTotal; idx = (idx + 1) % 75; if (idx == 0) bufferFull = true;
}

// ================================================================
// --- SETUP & LOOP ---
// ================================================================

void setup() {
    Serial.begin(115200);
    GPS_Serial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
    Wire.begin(I2C_SDA, I2C_SCL);
    
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.println("System Starting...");
    display.display();
    
    aht.begin(); bmp.begin(0x77); mpu.begin();
    
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
    if (SD_MMC.begin("/sdcard", true, true)) {
        if (!SD_MMC.exists("/camera")) SD_MMC.mkdir("/camera");
        updateSDSpace();
    }
    
    setupCamera(); 
    lastPressure = bmp.readPressure() / 100.0;
    lastMoveTime = millis();
}

void loop() {
    // 1. GPS 数据解析与时间同步
    while (GPS_Serial.available() > 0) {
        if (gps.encode(GPS_Serial.read())) {
            if (!timeSynced && gps.satellites.value() >= 3 && gps.date.isValid()) {
                struct tm tm_info;
                tm_info.tm_year = gps.date.year() - 1900;
                tm_info.tm_mon = gps.date.month() - 1;
                tm_info.tm_mday = gps.date.day();
                tm_info.tm_hour = gps.time.hour();
                tm_info.tm_min = gps.time.minute();
                tm_info.tm_sec = gps.time.second();
                time_t t = mktime(&tm_info);
                struct timeval now_tv = { .tv_sec = t };
                settimeofday(&now_tv, NULL);
                timeSynced = true;
            }
        }
    }

    unsigned long now = millis();
    isMoving = (gps.speed.kmph() > 2.5);

    // 2. 状态切换：移动 vs 省电
    if (isMoving) {
        lastMoveTime = now;
        if (isPowerSaving) { 
            setCpuFrequencyMhz(240); setupCamera(); 
            display.ssd1306_command(SSD1306_DISPLAYON); isPowerSaving = false; 
        }
    } else if (now - lastMoveTime > POWER_SAVE_DELAY && !isPowerSaving) {
        isPowerSaving = true; esp_camera_deinit();
        display.ssd1306_command(SSD1306_DISPLAYOFF); setCpuFrequencyMhz(80);
    }

    // 3. 坡度与冲击检测
    if (now - lastSlopeCheck > 15000) {
        float currentPressure = bmp.readPressure() / 100.0;
        if (lastPressure > 0) {
            float diff = currentPressure - lastPressure;
            if (diff < -0.15) slopeStatus = "Up";
            else if (diff > 0.15) slopeStatus = "Down";
            else slopeStatus = "Flat";
        }
        lastPressure = currentPressure; lastSlopeCheck = now;
    }
    checkShock();  

    // 4. 数据记录逻辑
    if (!isPowerSaving) {
        // --- 正常模式：5秒拍照 & 5秒记录 ---
        if (isMoving && (now - lastPhotoTime > PHOTO_INTERVAL)) capturePhoto();

        if (now - lastLogTime > (isMoving ? MOVE_LOG_INTERVAL : IDLE_LOG_INTERVAL)) {
            sensors_event_t h, t; aht.getEvent(&h, &t);
            String fileName = getDailyFileName("data.csv");
            File f = SD_MMC.open(fileName, FILE_APPEND);
            if (f) {
                if (f.size() == 0) f.println("UTC,Temp_C,Hum_%,Press_hPa,Slope,Lat,Lng,Speed_kmph");
                f.printf("%s,%.2f,%.1f,%.2f,%s,%.6f,%.6f,%.1f\n", 
                         getUTCString().c_str(), t.temperature, h.relative_humidity, 
                         bmp.readPressure()/100.0, slopeStatus.c_str(), 
                         gps.location.lat(), gps.location.lng(), gps.speed.kmph());
                f.close();
            }
            lastLogTime = now; updateSDSpace(); 
        }

        // --- OLED 显示 (略去翻页逻辑的详细重复，保持简洁) ---
        if (now % 500 < 50) { // 模拟 0.5秒刷新
            display.clearDisplay();
            display.setCursor(0, 0);
            if (showPhotoStatus && (now - photoStatusTimer < 1000)) {
                display.println("SAVING PHOTO...");
            } else {
                display.printf("T:%s %s\n", getUTCString().c_str(), timeSynced ? "*" : "!");
                display.printf("SPD:%.1f %s SAT:%d\n", gps.speed.kmph(), slopeStatus.c_str(), gps.satellites.value());
                display.printf("Free: %.2f GB", sdFreeGB);
            }
            display.display();
        }
    } else {
        // --- 省电模式：后台记录 (10分钟) ---
        if (now - lastLogTime > IDLE_LOG_INTERVAL) {
            sensors_event_t h, t; aht.getEvent(&h, &t);
            String fileName = getDailyFileName("idle.csv");
            File f = SD_MMC.open(fileName, FILE_APPEND);
            if (f) {
                if (f.size() == 0) f.println("UTC,Status,Temp_C,Hum_%");
                f.printf("%s,IDLE,%.2f,%.1f\n", getUTCString().c_str(), t.temperature, h.relative_humidity); 
                f.close(); 
            }
            lastLogTime = now;
        }
    }
}