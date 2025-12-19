#include "Relay_Button.h"
#include "LCD.h"
#include "Sensors.h"
#include "Logger.h"
#include <Arduino.h>

#define NANG_GAT_LUX 20000   // nắng gắt trong nhà màng

// ================== extern từ file khác ==================
// UI state
extern uint8_t g_lcdPage;          // từ LCD.cpp / .ino

extern int DC1;
extern int DC2;
extern int EN;

extern const int BTN_OPEN;
extern const int BTN_CLOSE;

// Sensor values (được cập nhật trong Sensors.cpp)
extern int   g_tempC;
extern int   g_hum; 
extern int   g_lux;
extern int   g_moist;
extern float g_ph;
extern bool  g_rain;

// Thời gian chuỗi "HH:MM:SS" từ RTC task
extern char g_time[9];

// Pump 5V Relay 3 schedule config (khai báo trong .ino)
extern int Pump_Mor_sec;
extern int Pump_Aft_sec;
extern int Pump_Dur_sec;

// Pump 12V Relay 4 schedule config (khai báo trong .ino)
extern int Pump2_Mor_sec;
extern int Pump2_Aft_sec;
extern int Pump2_Dur_sec;

// Ngưỡng cố định 24/7 khai báo trong .ino
extern int   Temp_High, Temp_Low;   // Fan
extern int Hum_High, Hum_Low;       // Fan
extern int   Lux_High,  Lux_Low;    // Light
extern int   Soil_High, Soil_Low;   // Pump

// ================== Ngưỡng pH ==================
static const float PH_MIN = 4.0f;
static const float PH_MAX = 7.0f;

// ================== Trạng thái MODE & RELAY ==================
volatile bool g_autoMode = false;   // true = AUTO, false = MANUAL
bool r1=false, r2=false, r3=false, r4=false;

// ===== MOTOR DC (mái che) =====
static unsigned long motorStart = 0;
static bool motorRunning = false;

enum MotorState {
  M_STOPPED,
  M_FORWARD,
  M_REVERSE
};

static MotorState motorState = M_STOPPED;

// Ngưỡng PWM tối thiểu (default 100, cho Blynk + Telegram chỉnh)
uint8_t g_roofPwmMin = 100;

// Trigger từ Blynk/Telegram (mô phỏng bấm nút)
static volatile bool roofOpenTrig  = false;
static volatile bool roofCloseTrig = false;

// ===== Tốc độ cố định: không dùng EN (biến trở) nữa =====
// Vẫn cho phép chỉnh qua g_roofPwmMin (Blynk/Telegram), nếu =0 thì dùng default.
static const uint8_t ROOF_DEFAULT_SPEED = 180;

static uint8_t motorCalcSpeed() {
  // Ví dụ: tốc độ cố định 180/255
  return 255;
}

static void motorForward(uint8_t duty) {
  analogWrite(DC1, duty);
  digitalWrite(DC2, LOW);
}

static void motorReverse(uint8_t duty) {
  analogWrite(DC2, duty);
  digitalWrite(DC1, LOW);
}

static void motorStop() {
  analogWrite(DC1, 0);
  analogWrite(DC2, 0);
  motorRunning = false;
  motorState   = M_STOPPED;
}

// ================== Button state (cơ bản, không mảng/struct) ==================
static bool lastBTN1  = false;
static bool lastBTN2  = false;
static bool lastBTN3  = false;
static bool lastBTN4  = false;
static bool lastMODE  = false;
static bool lastUI    = false;

// ================== Helper: button ==================
static inline bool isPressedLevel(int level) {
  // nếu ACTIVE_LOW thì nhấn = LOW
  return BTN_ACTIVE_LOW ? (level == LOW) : (level == HIGH);
}

static bool readBtn(uint8_t pin) {
  int lv = digitalRead(pin);
  return isPressedLevel(lv);
}

// rising = TRUE đúng 1 lần khi chuyển từ KHÔNG NHẤN -> NHẤN
static bool BTN1_Rising() {
  bool now = readBtn(BTN1);
  bool ret = (!lastBTN1 && now);
  lastBTN1 = now;
  return ret;
}

static bool BTN2_Rising() {
  bool now = readBtn(BTN2);
  bool ret = (!lastBTN2 && now);
  lastBTN2 = now;
  return ret;
}

static bool BTN3_Rising() {
  bool now = readBtn(BTN3);
  bool ret = (!lastBTN3 && now);
  lastBTN3 = now;
  return ret;
}

static bool BTN4_Rising() {
  bool now = readBtn(BTN4);
  bool ret = (!lastBTN4 && now);
  lastBTN4 = now;
  return ret;
}

static bool MODE_Rising() {
  bool now = readBtn(BTN_MODE);
  bool ret = (!lastMODE && now);
  lastMODE = now;
  return ret;
}

static bool UI_Rising() {
  bool now = readBtn(BTN_UI);
  bool ret = (!lastUI && now);
  lastUI = now;
  return ret;
}

void RB_TriggerRoofOpen() {
  roofOpenTrig  = true;
  roofCloseTrig = false;
}

void RB_TriggerRoofClose() {
  roofCloseTrig = true;
  roofOpenTrig  = false;
}

// ================== Helper: relay ==================
static inline void applyRelayPin(uint8_t pin, bool on) {
  digitalWrite(pin, on ? RELAY_ON : RELAY_OFF);
}

// set relay "thô" không log (dùng trong init)
static void setRelayRaw(uint8_t idx, bool on) {
  switch (idx) {
    case 1: r1 = on; applyRelayPin(RELAY1, r1); break;
    case 2: r2 = on; applyRelayPin(RELAY2, r2); break;
    case 3: r3 = on; applyRelayPin(RELAY3, r3); break;
    case 4: r4 = on; applyRelayPin(RELAY4, r4); break;
  }
}

// Helper: đổi g_time -> giây tính từ 00:00:00
static int secondsOfDayFrom_g_time() {
  if (g_time[2] != ':' || g_time[5] != ':') return -1;
  int hh = (g_time[0]-'0')*10 + (g_time[1]-'0');
  int mm = (g_time[3]-'0')*10 + (g_time[4]-'0');
  int ss = (g_time[6]-'0')*10 + (g_time[7]-'0');
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) return -1;
  return hh*3600 + mm*60 + ss;
}

// ================== API MODE ==================
void RB_SetAuto(bool on) {
  if (g_autoMode == on) return;
  g_autoMode = on;
  LCD_ForceRefresh();
  Logger_EnqueueEvent("", g_autoMode ? "MODE=AUTO" : "MODE=MANUAL", "");
}

bool RB_IsManual() { return !g_autoMode; }
bool RB_IsAuto()   { return  g_autoMode; }

// ================== API RELAY (có log) ==================
void RB_SetRelay1(bool on) {
  if (r1 == on) return;
  r1 = on;
  applyRelayPin(RELAY1, r1);
  Logger_EnqueueEvent("", r1 ? "R1=ON" : "R1=OFF", "");
}

void RB_SetRelay2(bool on) {
  if (r2 == on) return;
  r2 = on;
  applyRelayPin(RELAY2, r2);
  Logger_EnqueueEvent("", r2 ? "R2=ON" : "R2=OFF", "");
}

void RB_SetRelay3(bool on) {
  if (r3 == on) return;
  r3 = on;
  applyRelayPin(RELAY3, r3);
  Logger_EnqueueEvent("", r3 ? "R3=ON" : "R3=OFF", "");
}

void RB_SetRelay4(bool on) {
  if (r4 == on) return;
  r4 = on;
  applyRelayPin(RELAY4, r4);
  Logger_EnqueueEvent("", r4 ? "R4=ON" : "R4=OFF", "");
}

void RB_ToggleRelay1() { RB_SetRelay1(!r1); }
void RB_ToggleRelay2() { RB_SetRelay2(!r2); }
void RB_ToggleRelay3() { RB_SetRelay3(!r3); }
void RB_ToggleRelay4() { RB_SetRelay4(!r4); }

// ================== INIT ==================
void RB_Init() {
  // Relay outputs
  pinMode(RELAY1, OUTPUT);
  pinMode(RELAY2, OUTPUT);
  pinMode(RELAY3, OUTPUT);
  pinMode(RELAY4, OUTPUT);

  // Tắt hết relay ban đầu (không log)
  setRelayRaw(1, false);
  setRelayRaw(2, false);
  setRelayRaw(3, false);
  setRelayRaw(4, false);

  // Motor pins (cho chắc, dù EN không còn dùng để analogRead)
  pinMode(DC1, OUTPUT);
  pinMode(DC2, OUTPUT);
  pinMode(EN,  INPUT);

  // Buttons (ở .ino anh vẫn có thể giữ Buttons_PinInit, nhưng ở đây ta set lại chắc ăn)
  if (BTN_ACTIVE_LOW) {
    pinMode(BTN1,       INPUT_PULLUP);
    pinMode(BTN2,       INPUT_PULLUP);
    pinMode(BTN3,       INPUT_PULLUP);
    pinMode(BTN4,       INPUT_PULLUP);
    pinMode(BTN_MODE,   INPUT_PULLUP);
    pinMode(BTN_UI,     INPUT_PULLUP);
    pinMode(BTN_OPEN,   INPUT_PULLUP);
    pinMode(BTN_CLOSE,  INPUT_PULLUP);
  } else {
    pinMode(BTN1,       INPUT);
    pinMode(BTN2,       INPUT);
    pinMode(BTN3,       INPUT);
    pinMode(BTN4,       INPUT);
    pinMode(BTN_MODE,   INPUT);
    pinMode(BTN_UI,     INPUT);
    pinMode(BTN_OPEN,   INPUT);
    pinMode(BTN_CLOSE,  INPUT);
  }

  // Đọc trạng thái ban đầu nút để không bị rising ảo
  lastBTN1 = readBtn(BTN1);
  lastBTN2 = readBtn(BTN2);
  lastBTN3 = readBtn(BTN3);
  lastBTN4 = readBtn(BTN4);
  lastMODE = readBtn(BTN_MODE);
  lastUI   = readBtn(BTN_UI);
}

// ================== AUTO theo ngưỡng cố định ==================
static void RB_AutoApply() {
  bool fan   = r1;
  bool light = r2;
  bool pump  = r3;
  bool pump2 = r4;   // Pump2: pH + giờ riêng
  
  // --- Tính giây trong ngày để xét slot tưới giờ ---
  int secOfDay = secondsOfDayFrom_g_time();
  
  // Lịch tưới PUMP1
  bool schedPumpOn = false;
  if (secOfDay >= 0 && Pump_Dur_sec > 0) {
    int morEnd = Pump_Mor_sec + Pump_Dur_sec;
    int aftEnd = Pump_Aft_sec + Pump_Dur_sec;
    if (secOfDay >= Pump_Mor_sec && secOfDay < morEnd) schedPumpOn = true;
    if (secOfDay >= Pump_Aft_sec && secOfDay < aftEnd) schedPumpOn = true;
  }

  // Lịch tưới PUMP2 (riêng)
  bool schedPump2On = false;
  if (secOfDay >= 0 && Pump2_Dur_sec > 0) {
    int mor2End = Pump2_Mor_sec + Pump2_Dur_sec;
    int aft2End = Pump2_Aft_sec + Pump2_Dur_sec;
    if (secOfDay >= Pump2_Mor_sec && secOfDay < mor2End) schedPump2On = true;
    if (secOfDay >= Pump2_Aft_sec && secOfDay < aft2End) schedPump2On = true;
  }

  // --- Fan theo nhiệt độ ---
// ===== BẬT quạt =====
// Nóng HOẶC Không khí khô
if (!fan && (g_tempC >= Temp_High || g_hum <= Hum_Low)) {
    fan = true;
}

// ===== TẮT quạt =====
// Mát VÀ Không khí đủ ẩm
if (fan && (g_tempC <= Temp_Low && g_hum >= Hum_High)) {
    fan = false;
}
  // --- Light theo lux ---
  // Bật nếu ánh sáng YẾU (trời tối)
  if (!light && g_lux <= Lux_Low)  light = true;
  // Tắt nếu ánh sáng MẠNH (trời sáng)
  if ( light && g_lux >= Lux_High) light = false;

  // --- Pump1 theo soil moisture ---
  // Bật nếu Độ ẩm đất KHÔ
  if (!pump && g_moist <= Soil_Low)  pump = true;
  // Tắt nếu Độ ẩm đất ẨM
  if ( pump && g_moist >= Soil_High) pump = false;

  // ƯU TIÊN LỊCH: trong giờ tưới thì ép ON
  if (schedPumpOn) {
    pump = true;
  }

  if (schedPump2On && g_ph >= PH_MIN && g_ph <= PH_MAX) {
    pump2 = true;
  } else {
    pump2 = false;
  }
  
  // Áp ra relay (có log nếu thay đổi)
  RB_SetRelay1(fan);
  RB_SetRelay2(light);
  RB_SetRelay3(pump);
  RB_SetRelay4(pump2);

  // ==== MOTOR AUTO: MƯA (prio 1) + NẮNG GẮT (prio 2) ====
  static bool lastRain = false;   // lưu trạng thái mưa lần trước
  static bool lastSun  = false;

  bool sunGat = (g_lux >= NANG_GAT_LUX);
  uint8_t speed = motorCalcSpeed();

  if (!motorRunning) {
  
    // PRIORITY 1: MƯA → ĐÓNG
    if (g_rain && !lastRain) {
      motorReverse(speed);
      motorRunning = true;
      motorState   = M_REVERSE;
      motorStart   = millis();
    }
  
    // PRIORITY 2: NẮNG GẮT → ĐÓNG (khi không mưa)
    else if (!g_rain && sunGat && !lastSun) {
      motorReverse(speed);
      motorRunning = true;
      motorState   = M_REVERSE;
      motorStart   = millis();
    }
  
    // HẾT MƯA + HẾT NẮNG → MỞ
    else if (!g_rain && !sunGat && (lastRain || lastSun)) {
      motorForward(speed);
      motorRunning = true;
      motorState   = M_FORWARD;
      motorStart   = millis();
    }
  
    lastRain = g_rain;
    lastSun  = sunGat;
  }
  else {
    if (millis() - motorStart >= 1900) motorStop();
  }
}

// ================== Manual Mode ==================
void RB_TaskManual() {
  if (BTN1_Rising()) RB_ToggleRelay1();
  if (BTN2_Rising()) RB_ToggleRelay2();
  if (BTN3_Rising()) RB_ToggleRelay3();
  if (BTN4_Rising()) RB_ToggleRelay4();

  // ===== MOTOR MANUAL =====
  uint8_t speed = motorCalcSpeed();

  // Nếu motor đang dừng
  if (!motorRunning) {
    bool openPressed  = readBtn(BTN_OPEN)  || roofOpenTrig;
    bool closePressed = readBtn(BTN_CLOSE) || roofCloseTrig;

    if (openPressed) {
      motorForward(speed);
      motorRunning = true;
      motorState   = M_FORWARD;
      motorStart   = millis();
      roofOpenTrig = roofCloseTrig = false;  // clear trigger sau khi dùng
      // ==== LOG ====
      char msg[32];
      snprintf(msg, sizeof(msg), "PWM=%d", speed);
      Logger_EnqueueEvent("ROOF", "OPEN", msg);
    }
    else if (closePressed) {
      motorReverse(speed);
      motorRunning = true;
      motorState   = M_REVERSE;
      motorStart   = millis();
      roofOpenTrig = roofCloseTrig = false;
      // ==== LOG ====
      char msg[32];
      snprintf(msg, sizeof(msg), "PWM=%d", speed);
      Logger_EnqueueEvent("ROOF", "CLOSE", msg);
    }
  }
  else if (motorRunning) {
    if (millis() - motorStart >= 2000) {
      motorStop();
    }
  }
}

// ================== Tick ==================
void RB_Task() {
  // MODE: AUTO <-> MANUAL
  if (MODE_Rising()) {
    RB_SetAuto(!g_autoMode);
  }

  // UI: xoay vòng 3 màn hình LCD (0→1→2→0)
  if (UI_Rising()) {
    g_lcdPage = (g_lcdPage + 1) % 3;
    LCD_ForceRefresh();
  }

  // AUTO
  if (g_autoMode) {
    RB_AutoApply();
    return;
  }

  // MANUAL
  RB_TaskManual();

  btn_open_state  = readBtn(BTN_OPEN);
  btn_close_state = readBtn(BTN_CLOSE); 
}
