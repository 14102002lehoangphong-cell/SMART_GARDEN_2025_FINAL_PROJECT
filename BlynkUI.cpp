#define BLYNK_TEMPLATE_ID "TMPL6r1Ge1_Yd"
#define BLYNK_TEMPLATE_NAME "Đồ án giám sát vườn dưa lưới"

//#define BLYNK_TEMPLATE_ID   "TMPL68r-ZUxkz"
//#define BLYNK_TEMPLATE_NAME "TAIPHONG"


#include "BlynkUI.h"
#include <BlynkSimpleEsp32.h>
#include <WiFi.h>
#include "RTC.h"
#include "Telegram.h"

// ===== Externs từ các module khác =====
extern volatile bool g_autoMode;
extern bool  r1, r2, r3, r4;
extern int   g_tempC, g_hum, g_pres, g_lux, g_moist;
extern float g_ph;

extern char g_date[11], g_time[9];

// 6 ngưỡng cố định 24/7 (khai báo trong .ino)
extern int Temp_High;   // Quạt ON khi temp >= Temp_High
extern int Temp_Low;    // Quạt OFF khi temp <= Temp_Low
extern int Hum_High;    // Quạt ON khi temp >= Hum_High
extern int Hum_Low;     // Quạt OFF khi temp <= Hum_Low
extern int Lux_High;    // Đèn OFF khi lux >= Lux_High
extern int Lux_Low;     // Đèn ON  khi lux <= Lux_Low
extern int Soil_High;   // Bơm OFF khi ẩm đất >= Soil_High
extern int Soil_Low;    // Bơm ON  khi ẩm đất <= Soil_Low
extern bool g_buzzerEnabled;

// Pump schedule (seconds from midnight + duration)
extern int Pump_Mor_sec;
extern int Pump_Aft_sec;
extern int Pump_Dur_sec;

extern int Pump2_Mor_sec;
extern int Pump2_Aft_sec;
extern int Pump2_Dur_sec;

extern void Relays_Set(int idx, bool state);
extern bool Logger_EnqueueEvent(const char* event, const char* relay, const char* state);

extern uint8_t g_roofPwmMin;
extern void RB_TriggerRoofOpen();
extern void RB_TriggerRoofClose();

// ===== Map Virtual Pins cho sliders thresholds =====
#define VP_SOIL_LOW    V10
#define VP_SOIL_HIGH   V11
#define VP_LUX_LOW     V12
#define VP_LUX_HIGH    V13
#define VP_TEMP_LOW    V14
#define VP_TEMP_HIGH   V15
#define VP_HUM_LOW     V17
#define VP_HUM_HIGH    V18

// Công tắc THRESH EDIT ON/OFF
#define VP_THRESH_SWITCH  V16

// Pump1: Number Input giờ / phút + duration
#define VP_PUMP1_MOR_HOUR  V20   // Pump1 sáng - giờ (0–23)
#define VP_PUMP1_MOR_MIN   V21   // Pump1 sáng - phút (0–59)
#define VP_PUMP1_AFT_HOUR  V31   // Pump1 chiều - giờ (0–23)
#define VP_PUMP1_AFT_MIN   V32   // Pump1 chiều - phút (0–59)
#define VP_PUMP1_DUR_SEC   V22   // Pump1 duration (giây)

// Pump2: Number Input giờ / phút + duration
#define VP_PUMP2_MOR_HOUR  V26   // Pump2 sáng - giờ (0–23)
#define VP_PUMP2_MOR_MIN   V27   // Pump2 sáng - phút (0–59)
#define VP_PUMP2_AFT_HOUR  V33   // Pump2 chiều - giờ (0–23)
#define VP_PUMP2_AFT_MIN   V34   // Pump2 chiều - phút (0–59)
#define VP_PUMP2_DUR_SEC   V28   // Pump2 duration (giây)


#define VP_BUZZER_SWITCH  V23   // công tắc Buzzer ON/OFF

#define VP_ROOF_PWM_MIN   V24   // slider chỉnh g_roofPwmMin
#define VP_ROOF_OPEN_BTN  V29   // button open  (ĐỔI TỪ V25 -> V29)
#define VP_ROOF_CLOSE_BTN V30   // button close (ĐỔI TỪ V26 -> V30)

static BlynkTimer s_timer;

// ============================================================
//  PUMP1 - Debounce 3s rồi mới commit + gửi Telegram
// ============================================================

static int pend_p1_mor_h = -1, pend_p1_mor_m = -1;
static int pend_p1_aft_h = -1, pend_p1_aft_m = -1;
static int pend_p1_dur_s = -1;

static int tmrCommitP1 = -1;   // id timer để hủy/đặt lại

static void commitPump1_() {
  // Apply pending -> Pump_Mor_sec / Pump_Aft_sec / Pump_Dur_sec
  int h, m;

  // Morning
  h = (pend_p1_mor_h >= 0) ? pend_p1_mor_h : (Pump_Mor_sec / 3600);
  m = (pend_p1_mor_m >= 0) ? pend_p1_mor_m : ((Pump_Mor_sec % 3600) / 60);
  if (h < 0) h = 0; if (h > 23) h = 23;
  if (m < 0) m = 0; if (m > 59) m = 59;
  Pump_Mor_sec = h * 3600 + m * 60;

  // Afternoon
  h = (pend_p1_aft_h >= 0) ? pend_p1_aft_h : (Pump_Aft_sec / 3600);
  m = (pend_p1_aft_m >= 0) ? pend_p1_aft_m : ((Pump_Aft_sec % 3600) / 60);
  if (h < 0) h = 0; if (h > 23) h = 23;
  if (m < 0) m = 0; if (m > 59) m = 59;
  Pump_Aft_sec = h * 3600 + m * 60;

  // Duration
  if (pend_p1_dur_s >= 0) {
    int s = pend_p1_dur_s;
    if (s < 0) s = 0;
    if (s > 3600) s = 3600;   // max 1h
    Pump_Dur_sec = s;
  }

  // Reset pending
  pend_p1_mor_h = pend_p1_mor_m = -1;
  pend_p1_aft_h = pend_p1_aft_m = -1;
  pend_p1_dur_s = -1;

  // Sync lại UI để chắc chắn hiển thị đúng
  BlynkUI_SyncThresholdSliders();

  // Gửi Telegram báo cập nhật
  char msg[220];
  int mh = Pump_Mor_sec / 3600, mm = (Pump_Mor_sec % 3600) / 60;
  int ah = Pump_Aft_sec / 3600, am = (Pump_Aft_sec % 3600) / 60;

  snprintf(msg, sizeof(msg),
           "✅ Pump1 schedule updated\n"
           "• Morning: %02d:%02d\n"
           "• Afternoon: %02d:%02d\n"
           "• Duration: %d s",
           mh, mm, ah, am, Pump_Dur_sec);

  Telegram_Enqueue(msg);

  // Mark done
  tmrCommitP1 = -1;
}

static void scheduleCommitPump1_() {
  // Reset timer 3s mỗi lần chỉnh
  if (tmrCommitP1 != -1) {
    s_timer.deleteTimer(tmrCommitP1);
    tmrCommitP1 = -1;
  }
  tmrCommitP1 = s_timer.setTimeout(3000L, commitPump1_);
}

// ============================================================
//  PUMP2 - Debounce 3s rồi mới commit + gửi Telegram
// ============================================================

static int pend_p2_mor_h = -1, pend_p2_mor_m = -1;
static int pend_p2_aft_h = -1, pend_p2_aft_m = -1;
static int pend_p2_dur_s = -1;

static int tmrCommitP2 = -1;

static void commitPump2_() {
  int h, m;

  // Morning
  h = (pend_p2_mor_h >= 0) ? pend_p2_mor_h : (Pump2_Mor_sec / 3600);
  m = (pend_p2_mor_m >= 0) ? pend_p2_mor_m : ((Pump2_Mor_sec % 3600) / 60);
  if (h < 0) h = 0; if (h > 23) h = 23;
  if (m < 0) m = 0; if (m > 59) m = 59;
  Pump2_Mor_sec = h * 3600 + m * 60;

  // Afternoon
  h = (pend_p2_aft_h >= 0) ? pend_p2_aft_h : (Pump2_Aft_sec / 3600);
  m = (pend_p2_aft_m >= 0) ? pend_p2_aft_m : ((Pump2_Aft_sec % 3600) / 60);
  if (h < 0) h = 0; if (h > 23) h = 23;
  if (m < 0) m = 0; if (m > 59) m = 59;
  Pump2_Aft_sec = h * 3600 + m * 60;

  // Duration
  if (pend_p2_dur_s >= 0) {
    int s = pend_p2_dur_s;
    if (s < 0) s = 0;
    if (s > 3600) s = 3600;
    Pump2_Dur_sec = s;
  }

  // reset pending
  pend_p2_mor_h = pend_p2_mor_m = -1;
  pend_p2_aft_h = pend_p2_aft_m = -1;
  pend_p2_dur_s = -1;

  // Sync UI
  BlynkUI_SyncThresholdSliders();

  // Telegram notify (nhắc rõ: Pump2 vẫn chạy theo điều kiện pH)
  char msg[240];
  int mh = Pump2_Mor_sec / 3600, mm = (Pump2_Mor_sec % 3600) / 60;
  int ah = Pump2_Aft_sec / 3600, am = (Pump2_Aft_sec % 3600) / 60;

  snprintf(msg, sizeof(msg),
           "✅ Pump2 schedule updated\n"
           "• Morning: %02d:%02d\n"
           "• Afternoon: %02d:%02d\n"
           "• Duration: %d s\n"
           "ℹ️ Pump2 runs only if pH is OK",
           mh, mm, ah, am, Pump2_Dur_sec);

  Telegram_Enqueue(msg);

  tmrCommitP2 = -1;
}

static void scheduleCommitPump2_() {
  if (tmrCommitP2 != -1) {
    s_timer.deleteTimer(tmrCommitP2);
    tmrCommitP2 = -1;
  }
  tmrCommitP2 = s_timer.setTimeout(3000L, commitPump2_);
}


static int secondsFromHHMM(int hhmm) {
  int hh = hhmm / 100;
  int mm = hhmm % 100;
  if (hh < 0) hh = 0; if (hh > 23) hh = 23;
  if (mm < 0) mm = 0; if (mm > 59) mm = 59;
  return hh*3600 + mm*60;
}
static int hhmmFromSeconds(int sec) {
  if (sec < 0) return 0;
  int hh = sec / 3600;
  int mm = (sec % 3600) / 60;
  return hh*100 + mm;   // ví dụ 7h30 -> 730
}

static int  last_mode=-1, last_r1=-1, last_r2=-1, last_r3=-1, last_r4=-1;
static char lastDate[11] = "--/--/----";

// --- Gửi gauge (V50..V54) mỗi 15 giây ---
static void pushGaugesToBlynk() {
  if (!Blynk.connected()) return;
  Blynk.virtualWrite(V50, g_tempC);
  Blynk.virtualWrite(V51, g_moist);
  Blynk.virtualWrite(V52, g_lux);
  Blynk.virtualWrite(V53, g_hum);
  Blynk.virtualWrite(V54, g_ph);
}

// --- Gửi trạng thái mode/relay (V1..V5) nhanh 0.5–1 giây/lần ---
static void pushStatusToBlynk() {
  if (!Blynk.connected()) return;

  int mode = g_autoMode ? 1 : 0;
  if (mode != last_mode) { last_mode = mode; Blynk.virtualWrite(V1, mode); }
  int v2 = r1 ? 1 : 0; if (v2 != last_r1) { last_r1 = v2; Blynk.virtualWrite(V2, v2); }
  int v3 = r2 ? 1 : 0; if (v3 != last_r2) { last_r2 = v3; Blynk.virtualWrite(V3, v3); }
  int v4 = r3 ? 1 : 0; if (v4 != last_r3) { last_r3 = v4; Blynk.virtualWrite(V4, v4); }
  int v5 = r4 ? 1 : 0; if (v5 != last_r4) { last_r4 = v5; Blynk.virtualWrite(V5, v5); }
}

// --- Mỗi ngày mới: cập nhật lastDate + (tuỳ chọn) gửi header Telegram ---
static void tickDateChange() {
  if (g_date[2]=='/' && g_date[5]=='/') {
    if (strncmp(lastDate, g_date, 10) != 0) {
      strncpy(lastDate, g_date, 10);
      lastDate[10] = '\0';

      char msg[260];
      snprintf(msg, sizeof(msg),
        "📅 NEW DAY — %s\n"
        "Mode: %s\n"
        "Thresholds:\n"
        "  Temp: %d → %d °C\n"
        "  Lux : %d → %d\n"
        "  Soil: %d → %d %%\n"
        "F/L/P1/P2: %d/%d/%d/%d",
        g_date,
        g_autoMode ? "AUTO" : "MANUAL",
        Temp_Low, Temp_High,
        Lux_Low,  Lux_High,
        Soil_Low, Soil_High,
        r1?1:0, r2?1:0, r3?1:0, r4?1:0
      );
    }
  }
}

// ===== Hàm public: set trạng thái công tắc THRESH EDIT trên Blynk =====
void BlynkUI_SetThreshEditSwitch(bool on) {
  if (!Blynk.connected()) return;
  Blynk.virtualWrite(VP_THRESH_SWITCH, on ? 1 : 0);
}

// ===== Khi Blynk kết nối =====
BLYNK_CONNECTED() {
  Blynk.syncAll();

  // Sync trạng thái relay
  Blynk.virtualWrite(V2, r1 ? 1 : 0);
  Blynk.virtualWrite(V3, r2 ? 1 : 0);
  Blynk.virtualWrite(V4, r3 ? 1 : 0);
  Blynk.virtualWrite(V5, r4 ? 1 : 0);

  // Đẩy thresholds hiện tại ra sliders
  Blynk.virtualWrite(VP_TEMP_HIGH, Temp_High);
  Blynk.virtualWrite(VP_TEMP_LOW,  Temp_Low);
  Blynk.virtualWrite(VP_HUM_HIGH, Hum_High);
  Blynk.virtualWrite(VP_HUM_LOW,  Hum_Low);
  Blynk.virtualWrite(VP_LUX_HIGH,  Lux_High);
  Blynk.virtualWrite(VP_LUX_LOW,   Lux_Low);
  Blynk.virtualWrite(VP_SOIL_HIGH, Soil_High);
  Blynk.virtualWrite(VP_SOIL_LOW,  Soil_Low);
  
  // Sync trạng thái THRESH EDIT switch từ Telegram
  Blynk.virtualWrite(VP_THRESH_SWITCH, Telegram_IsThreshEditEnabled() ? 1 : 0);
  Blynk.virtualWrite(VP_BUZZER_SWITCH, g_buzzerEnabled ? 1 : 0);

  Blynk.virtualWrite(VP_ROOF_PWM_MIN, g_roofPwmMin);
  
  // Reset V99 (nút reboot)
  Blynk.virtualWrite(V99, 0);

  // Đẩy gauge ngay khi vừa kết nối để có số liệu ban đầu
  pushGaugesToBlynk();
}

// ===== Mode AUTO/MANUAL (V1) =====
BLYNK_WRITE(V1) {
  g_autoMode = (param.asInt() == 1);
}

// ===== Điều khiển 4 relay (V2..V5) =====
BLYNK_WRITE(V2){
  if (!g_autoMode) {
    Relays_Set(1, param.asInt()==1);
    Logger_EnqueueEvent("RELAY","1", r1?"ON":"OFF");
  }
  Blynk.virtualWrite(V2, r1?1:0);
}

BLYNK_WRITE(V3){
  if (!g_autoMode) {
    Relays_Set(2, param.asInt()==1);
    Logger_EnqueueEvent("RELAY","2", r2?"ON":"OFF");
  }
  Blynk.virtualWrite(V3, r2?1:0);
}

BLYNK_WRITE(V4){
  if (!g_autoMode) {
    Relays_Set(3, param.asInt()==1);
    Logger_EnqueueEvent("RELAY","3", r3?"ON":"OFF");
  }
  Blynk.virtualWrite(V4, r3?1:0);
}

BLYNK_WRITE(V5){
  if (!g_autoMode) {
    Relays_Set(4, param.asInt()==1);
    Logger_EnqueueEvent("RELAY","4", r4?"ON":"OFF");
  }
  Blynk.virtualWrite(V5, r4?1:0);
}

// ===== Sliders thresholds (V10..V15) — chỉnh trực tiếp 6 biến =====
// ===== Sliders thresholds (V10..V15) — chỉ cho đổi khi THRESH EDIT = ON =====
BLYNK_WRITE(VP_SOIL_LOW)  {
  if (!Telegram_IsThreshEditEnabled()) {
    // THRESH OFF → đẩy lại giá trị cũ lên app, kéo xong sẽ bật trở lại
    Blynk.virtualWrite(VP_SOIL_LOW, Soil_Low);
    return;
  }
  Soil_Low = param.asInt();
}

BLYNK_WRITE(VP_SOIL_HIGH) {
  if (!Telegram_IsThreshEditEnabled()) {
    Blynk.virtualWrite(VP_SOIL_HIGH, Soil_High);
    return;
  }
  Soil_High = param.asInt();
}

BLYNK_WRITE(VP_LUX_LOW) {
  if (!Telegram_IsThreshEditEnabled()) {
    Blynk.virtualWrite(VP_LUX_LOW, Lux_Low);
    return;
  }
  Lux_Low = param.asInt();
}

BLYNK_WRITE(VP_LUX_HIGH) {
  if (!Telegram_IsThreshEditEnabled()) {
    Blynk.virtualWrite(VP_LUX_HIGH, Lux_High);
    return;
  }
  Lux_High = param.asInt();
}

BLYNK_WRITE(VP_TEMP_LOW) {
  if (!Telegram_IsThreshEditEnabled()) {
    Blynk.virtualWrite(VP_TEMP_LOW, Temp_Low);
    return;
  }
  Temp_Low = param.asInt();
}

BLYNK_WRITE(VP_TEMP_HIGH) {
  if (!Telegram_IsThreshEditEnabled()) {
    Blynk.virtualWrite(VP_TEMP_HIGH, Temp_High);
    return;
  }
  Temp_High = param.asInt();
}

BLYNK_WRITE(VP_HUM_LOW) {
  if (!Telegram_IsThreshEditEnabled()) {
    Blynk.virtualWrite(VP_HUM_LOW, Hum_Low);
    return;
  }
  Hum_Low = param.asInt();
}

BLYNK_WRITE(VP_HUM_HIGH) {
  if (!Telegram_IsThreshEditEnabled()) {
    Blynk.virtualWrite(VP_HUM_HIGH, Hum_High);
    return;
  }
  Hum_High = param.asInt();
}

// ===== Pump1 schedule (V20/V21/V31/V32/V22) - debounce 3s =====
BLYNK_WRITE(VP_PUMP1_MOR_HOUR) {
  pend_p1_mor_h = param.asInt();
  scheduleCommitPump1_();
}

BLYNK_WRITE(VP_PUMP1_MOR_MIN) {
  pend_p1_mor_m = param.asInt();
  scheduleCommitPump1_();
}

BLYNK_WRITE(VP_PUMP1_AFT_HOUR) {
  pend_p1_aft_h = param.asInt();
  scheduleCommitPump1_();
}

BLYNK_WRITE(VP_PUMP1_AFT_MIN) {
  pend_p1_aft_m = param.asInt();
  scheduleCommitPump1_();
}

BLYNK_WRITE(VP_PUMP1_DUR_SEC) {
  pend_p1_dur_s = param.asInt();
  scheduleCommitPump1_();
}

// ===== Pump2 schedule (V26/V27/V33/V34/V28) - debounce 3s =====
BLYNK_WRITE(VP_PUMP2_MOR_HOUR) {
  pend_p2_mor_h = param.asInt();
  scheduleCommitPump2_();
}

BLYNK_WRITE(VP_PUMP2_MOR_MIN) {
  pend_p2_mor_m = param.asInt();
  scheduleCommitPump2_();
}

BLYNK_WRITE(VP_PUMP2_AFT_HOUR) {
  pend_p2_aft_h = param.asInt();
  scheduleCommitPump2_();
}

BLYNK_WRITE(VP_PUMP2_AFT_MIN) {
  pend_p2_aft_m = param.asInt();
  scheduleCommitPump2_();
}

BLYNK_WRITE(VP_PUMP2_DUR_SEC) {
  pend_p2_dur_s = param.asInt();
  scheduleCommitPump2_();
}


// ===== V16: Công tắc THRESH EDIT (ON/OFF) =====
BLYNK_WRITE(VP_THRESH_SWITCH) {
  bool en = (param.asInt() == 1);
  // Gọi sang Telegram để update state + gửi alert
  Telegram_SetThreshEditEnabled(en);
}

// ===== V23: Công tắc BUZZER ON/OFF =====
BLYNK_WRITE(VP_BUZZER_SWITCH) {
  int v = param.asInt();       // 0 hoặc 1
  g_buzzerEnabled = (v == 1);  // 1 = bật buzzer, 0 = tắt
}


BLYNK_WRITE(VP_ROOF_PWM_MIN) {
  int v = param.asInt();
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  g_roofPwmMin = (uint8_t)v;
}

BLYNK_WRITE(VP_ROOF_OPEN_BTN) {
  if (param.asInt() == 1) {
    RB_TriggerRoofOpen();
  }
}

BLYNK_WRITE(VP_ROOF_CLOSE_BTN) {
  if (param.asInt() == 1) {
    RB_TriggerRoofClose();
  }
}
// ===== Hàm public: sync thresholds lên sliders (cho Telegram gọi) =====
// ===== Hàm public: sync thresholds + schedule lên Blynk (cho Telegram gọi) =====
void BlynkUI_SyncThresholdSliders() {
  if (!Blynk.connected()) return;

  // Threshold sliders
  Blynk.virtualWrite(VP_TEMP_HIGH, Temp_High);
  Blynk.virtualWrite(VP_TEMP_LOW,  Temp_Low);
  Blynk.virtualWrite(VP_HUM_HIGH, Hum_High);
  Blynk.virtualWrite(VP_HUM_LOW,  Hum_Low);
  Blynk.virtualWrite(VP_LUX_HIGH,  Lux_High);
  Blynk.virtualWrite(VP_LUX_LOW,   Lux_Low);
  Blynk.virtualWrite(VP_SOIL_HIGH, Soil_High);
  Blynk.virtualWrite(VP_SOIL_LOW,  Soil_Low);

  // Switch states
  Blynk.virtualWrite(VP_THRESH_SWITCH, Telegram_IsThreshEditEnabled() ? 1 : 0);
  Blynk.virtualWrite(VP_BUZZER_SWITCH, g_buzzerEnabled ? 1 : 0);

  // Pump1
  int h = Pump_Mor_sec / 3600;
  int m = (Pump_Mor_sec % 3600) / 60;
  Blynk.virtualWrite(VP_PUMP1_MOR_HOUR, h);
  Blynk.virtualWrite(VP_PUMP1_MOR_MIN,  m);

  h = Pump_Aft_sec / 3600;
  m = (Pump_Aft_sec % 3600) / 60;
  Blynk.virtualWrite(VP_PUMP1_AFT_HOUR, h);
  Blynk.virtualWrite(VP_PUMP1_AFT_MIN,  m);
  Blynk.virtualWrite(VP_PUMP1_DUR_SEC,  Pump_Dur_sec);

  // Pump2
  h = Pump2_Mor_sec / 3600;
  m = (Pump2_Mor_sec % 3600) / 60;
  Blynk.virtualWrite(VP_PUMP2_MOR_HOUR, h);
  Blynk.virtualWrite(VP_PUMP2_MOR_MIN,  m);

  h = Pump2_Aft_sec / 3600;
  m = (Pump2_Aft_sec % 3600) / 60;
  Blynk.virtualWrite(VP_PUMP2_AFT_HOUR, h);
  Blynk.virtualWrite(VP_PUMP2_AFT_MIN,  m);
  Blynk.virtualWrite(VP_PUMP2_DUR_SEC,  Pump2_Dur_sec);
}

void BlynkUI_SetBuzzerSwitch(bool on) {
  if (!Blynk.connected()) return;
  Blynk.virtualWrite(VP_BUZZER_SWITCH, on ? 1 : 0);
}

// ===== V99: Nút reboot & sync NTP =====
BLYNK_WRITE(V99){
  if (param.asInt() == 1) {
    Blynk.virtualWrite(V99, 0);

    // Thử sync NTP nhanh
    RTC_SyncNTP(5000);

    // Ghi ngược DS3231 theo giờ địa phương
    struct tm ti;
    if (getLocalTime(&ti, 300) && RTC_Present()) {
      rtc.adjust(DateTime(ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                          ti.tm_hour, ti.tm_min, ti.tm_sec));
    }

    Blynk.run();
    delay(200);
    Telegram_MarkReboot("blynk");
    delay(100);
    ESP.restart();
  }
}

void BlynkUI_Init(const char* auth, const char* host, uint16_t port) {
  Blynk.config(auth);
  if (host && port) {
    Blynk.config(auth, host, port);
  }
  Blynk.connect(5000);

  s_timer.setInterval(500,    pushStatusToBlynk);  // mode/relay
  s_timer.setInterval(15000,  pushGaugesToBlynk);  // sensor gauges (15s)
  s_timer.setInterval(60000,  tickDateChange);     // check đổi ngày
}

void BlynkUI_Run() {
  Blynk.run();
  s_timer.run();
}
