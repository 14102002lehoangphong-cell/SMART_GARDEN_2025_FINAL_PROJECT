// ==================== BLYNK (MACRO STYLE) ====================
#define BLYNK_TEMPLATE_ID "TMPL6r1Ge1_Yd"
#define BLYNK_TEMPLATE_NAME "Đồ án giám sát vườn dưa lưới"
#define BLYNK_AUTH_TOKEN "Bpb97VfXR_tOqEcwWSz3WUIeLH6tcjtT"

//#define BLYNK_TEMPLATE_ID   "TMPL68r-ZUxkz"
//#define BLYNK_TEMPLATE_NAME "TAIPHONG"
//#define BLYNK_AUTH_TOKEN    "T0m5G_gHG5M-m6hndp7qnrtL9vK903c5"

// ===================== Core includes =====================
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>

#include "LCD.h"
#include "RTC.h"
#include "Relay_Button.h"
#include "Sensors.h"
#include "Logger.h"
#include "BlynkUI.h"   // Blynk UI (token dùng macro ở trên)
#include "Telegram.h"
#include "Alerts.h"

// ===================== Hardware mapping (EDIT to match wiring) =====================
// Relays (active level set by RELAY_ON/RELAY_OFF)
const int RELAY1 = 14;
const int RELAY2 = 27;
const int RELAY3 = 26;
const int RELAY4 = 25;
// NOTE: nếu relay board của bạn active-LOW (rất phổ biến), đổi lại:
// const int RELAY_ON = LOW; const int RELAY_OFF = HIGH;
const int RELAY_ON  = HIGH;
const int RELAY_OFF = LOW;

// Buttons (BTN_ACTIVE_LOW=true uses INPUT_PULLUP)
const int BTN1     = 23;  // Relay1 toggle
const int BTN2     = 19;  // Relay2 toggle
const int BTN3     = 18;  // Relay3 toggle
const int BTN4     = 5;   // Relay4 toggle
const int BTN_MODE = 17;  // Auto <-> Manual
const int BTN_UI   = 16;  // UI: chuyển trang LCD

// ==== Door Control Buttons ====
const int BTN_OPEN  = 4;   // was BTN_UP_DC
const int BTN_CLOSE = 13;  // was BTN_DOWN_DC

// I2C pins for LCD + RTC + BME280 + BH1750 (SDA=21, SCL=22)
#ifndef RTC_SDA_PIN
#define RTC_SDA_PIN 21
#endif
#ifndef RTC_SCL_PIN
#define RTC_SCL_PIN 22
#endif

// Soil calibration (adjust to your sensor)
const int SOIL_PIN     = 34;
const int PH_PIN       = 35;
const int RAIN_D0_PIN  = 36;
const int SOIL_RAW_DRY = 3200;
const int SOIL_RAW_WET = 1200;

// === Buzzer ===
int BUZZER_PIN = 2;    // D2 trên board ESP32
bool g_buzzerEnabled = false; // true = cho phép kêu khi có alert

// Motor Door
int DC1 = 32;   // Motor điều khiển chiều 1
int DC2 = 33;   // Motor điều khiển chiều 2
int EN  = 15;   // Đọc analog để ra tốc độ PWM 0–255

void Buttons_PinInit() {
  pinMode(BTN1, INPUT);
  pinMode(BTN2, INPUT);
  pinMode(BTN3, INPUT);
  pinMode(BTN4, INPUT);

  pinMode(BTN_MODE, INPUT);
  pinMode(BTN_UI, INPUT);
  
  pinMode(BTN_OPEN,  INPUT);
  pinMode(BTN_CLOSE, INPUT);
}

// ===================== Globals required by modules =====================
LiquidCrystal_I2C lcd(0x27, 20, 4);
RTC_DS3231 rtc;

// Nếu Sensors.cpp đã define bme/lightMeter thì xóa 2 dòng dưới:
Adafruit_BME280 bme;
BH1750         lightMeter;

char g_time[9]  = "--:--:--";     // HH:MM:SS
char g_date[11] = "--/--/----";   // DD/MM/YYYY

// NTP/Timezone (RTC.cpp có thể gọi configTime)
const char* ntpServer          = "time.google.com";
const long  gmtOffset_sec      = 7 * 3600;
const int   daylightOffset_sec = 0;

// UI/sensor state required by LCD.*
uint8_t g_lcdPage = 0;  // 0 = main, 1 = sensor, 2 = auto config

int   g_moist = 0;
int   g_tempC = 0;
int   g_pres  = 0;
int   g_hum   = 0;
int   g_lux   = 0;
float g_ph    = 7.0f;   // giá trị mặc định
bool  g_rain  = false;

//trạng thái nút OPEN/CLOSE để Logger & Blynk dùng
bool btn_open_state  = false;
bool btn_close_state = false;

// ====== THRESHOLDS — dùng cố định 24/7 ======3
int Temp_High = 28;   // Quạt ON khi temp >= 28°C
int Temp_Low  = 22;   // Quạt OFF khi temp <= 22°C

int Hum_High = 80;   // Fan ON nếu hum >= 80%
int Hum_Low  = 70;   // Fan OFF nếu hum <= 70%

int Lux_High  = 400;  // Đèn OFF khi lux >= 400
int Lux_Low   = 100;  // Đèn ON  khi lux <= 100

int Soil_High = 55;   // Bơm OFF khi ẩm đất >= 55%
int Soil_Low  = 40;   // Bơm ON  khi ẩm đất <= 40%

// ====== Pump 5V Relay 3 schedule (giờ tưới cố định mỗi ngày) ======
int Pump_Mor_sec = 7 * 3600 + 30 * 60;   // 07:30
int Pump_Aft_sec = 17 * 3600 + 30 * 60;  // 17:30
int Pump_Dur_sec = 420;                  // thời gian tưới (giây)

// ====== Pump 12V Relay 4 schedule (giờ bơm cố định mỗi ngày) ======
int Pump2_Mor_sec = 8 * 3600;   // ví dụ tưới 08:00
int Pump2_Aft_sec = 18 * 3600;  // ví dụ tưới 18:00
int Pump2_Dur_sec = 600;        // ví dụ 10 phút

// ===================== Wi-Fi (EDIT) =====================
// const char* WIFI_SSID = "416/18";
// const char* WIFI_PASS = "camonquykhach";
//const char* WIFI_SSID = "Turtle";
//const char* WIFI_PASS = "Tung@2000";
//const char* WIFI_SSID = "DOPECOFFEE134";
//const char* WIFI_PASS = "Dopecoffee134";
//const char* WIFI_SSID = "REDMI14";
//const char* WIFI_PASS = "00000000";
const char* WIFI_SSID = "Phong01";
const char* WIFI_PASS = "Tai@185202";

// ===================== Telegram credentials (EDIT) =====================
//const char* TG_BOT_TOKEN = "8418802941:AAGzHIhO-6eS_G9dBCzTDp1bGKTcEtldYFQ";
//const char* TG_CHAT_ID   = "5584571280";
const char* TG_BOT_TOKEN = "8418802941:AAGzHIhO-6eS_G9dBCzTDp1bGKTcEtldYFQ";
const char* TG_CHAT_ID   = "5584571280";

// ===================== Google Sheets Logger (EDIT URL) =====================
const char* SHEETS_URL    = "https://script.google.com/macros/s/AKfycbxxWBcHKcdTl8uz03j9adW5ltvo6H-naNTv2UWRfWAGEsjp6yQ5PF2OcQoLFGi8-Xoc/exec";
const char* SHEETS_SECRET = "TaiPhong";
const uint32_t LOG_PERIOD_MS = 5000;   // 5s/snapshot

// Điều khiển relay từ Blynk / Telegram (chỉ chạy khi MANUAL; AUTO bị khóa trong BlynkUI.cpp)
void Relays_Set(int idx, bool state) {
  switch (idx) {
    case 1: RB_SetRelay1(state); break;
    case 2: RB_SetRelay2(state); break;
    case 3: RB_SetRelay3(state); break;
    case 4: RB_SetRelay4(state); break;
  }
}

// (Tuỳ chọn) Hai helper Wi-Fi dùng chung
bool connectWiFiWithTimeout(uint32_t timeout_ms) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeout_ms) {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}
void powerOffWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ===================== Tasks =====================
void TaskButtons(void* pv) {
  (void)pv;
  for (;;) {
    RB_Task();               // handle mode/ui toggles + manual relay toggles
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ===================== Setup/Loop =====================
static void connectWiFi(uint32_t ms = 10000) {
  connectWiFiWithTimeout(ms);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nBOOT");

  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);   // mặc định tắt còi  

  // Init subsystems
  LCD_Init();
  Buttons_PinInit();
  RB_Init();
  RTC_Init();
  Sensors_Init();
  connectWiFi(8000);
  RTC_SyncNTP(15000);

  // Blynk init (nếu có Wi-Fi) — dùng token macro
  if (WiFi.status() == WL_CONNECTED) {
    BlynkUI_Init(BLYNK_AUTH_TOKEN, "blynk.cloud", 80);
  }

  // ===== Telegram init + opening =====
  Telegram_Init(TG_BOT_TOKEN, TG_CHAT_ID);
  Telegram_SendPendingRebootNotice();  // gửi thông báo reset nếu có
  // Gửi mở màn (chờ TaskRTC fill g_date/g_time)
  delay(5000);
  Telegram_SendHelloOnce();

  // Start tasks
  xTaskCreatePinnedToCore(TaskRTC,      "RTC",      4096,  nullptr, 1, nullptr, APP_CPU_NUM);
  xTaskCreatePinnedToCore(TaskLCD,      "LCD",      4096,  nullptr, 1, nullptr, APP_CPU_NUM);
  xTaskCreatePinnedToCore(TaskButtons,  "Buttons",  3072,  nullptr, 3, nullptr, APP_CPU_NUM);
  xTaskCreatePinnedToCore(TaskSensors,  "Sensors",  4096,  nullptr, 1, nullptr, APP_CPU_NUM);
  xTaskCreatePinnedToCore(TaskLogger,   "Logger",   12288, nullptr, 2, nullptr, APP_CPU_NUM);
  xTaskCreatePinnedToCore(TaskTelegram, "Telegram", 12288, nullptr, 3, nullptr, APP_CPU_NUM);
  xTaskCreatePinnedToCore(TaskAlerts,   "Alerts",   12288, nullptr, 1, nullptr, APP_CPU_NUM);

  LCD_ForceRefresh();
}

void loop() {
  // vòng Blynk (AUTO khóa relay tay đã xử lý sẵn trong BlynkUI.cpp)
  BlynkUI_Run();
  vTaskDelay(pdMS_TO_TICKS(10));
}
