#include <Arduino.h>
#include <stdarg.h>

// ===== Alert NẮNG GẮT =====
static bool warnSun = false;
static uint32_t lastSunMs = 0;

// Ngưỡng nắng gắt cố định
#define NANG_GAT_LUX 20000

// ==== externs ====
// Giá trị cảm biến
extern int   g_tempC, g_hum, g_pres, g_lux, g_moist;
extern char  g_date[11], g_time[9];
extern volatile bool g_autoMode;
extern bool  r1, r2, r3, r4;
extern float g_ph;
extern bool  g_rain;

// Ngưỡng cố định 24/7 (khai báo trong .ino)
extern int Temp_High;   // Quạt ON khi temp >= Temp_High
extern int Temp_Low;    // Quạt OFF khi temp <= Temp_Low
extern int Lux_High;    // Đèn OFF khi lux >= Lux_High
extern int Lux_Low;     // Đèn ON  khi lux <= Lux_Low
extern int Soil_High;   // Bơm OFF khi ẩm đất >= Soil_High
extern int Soil_Low;    // Bơm ON  khi ẩm đất <= Soil_Low

extern int BUZZER_PIN;
extern bool g_buzzerEnabled;

// Ngưỡng pH dùng chung với AUTO
static const float PH_MIN = 4.0f;
static const float PH_MAX = 7.0f;

extern bool Telegram_Enqueue(const char* msg);

// ===== Mirror MODE/RELAY =====
static inline const char* relayName(int idx){
  switch(idx){
    case 1: return "Fan";
    case 2: return "Light";
    case 3: return "Pump";
    case 4: return "Pump2";
    default: return "?";
  }
}

static void fmtStamp(char* out, size_t n){
  if (g_time[2]==':' && g_time[5]==':')
    snprintf(out, n, "%s %s", g_date, g_time);
  else
    snprintf(out, n, "N/A");
}

static void sendModeAndRelayDeltaOnce(bool oldAuto, bool newAuto, bool oldR[4], bool newR[4]) {
  char line[220]; 
  line[0] = '\0';
  bool any = false;

  for (int i = 0; i < 4; i++) {
    if (oldR[i] != newR[i]) {
      any = true;
      char seg[42];
      snprintf(seg, sizeof(seg), "%s %s→%s  ",
               relayName(i + 1),
               oldR[i] ? "ON" : "OFF",
               newR[i] ? "ON" : "OFF");
      strncat(line, seg, sizeof(line) - strlen(line) - 1);
    }
  }

  char msg[360];

  if (any) {
    snprintf(msg, sizeof(msg),
      "⚙️ MODE: %s → %s\n🔁 Relays: %s",
      oldAuto ? "AUTO" : "MANUAL",
      newAuto ? "AUTO" : "MANUAL",
      line
    );
  } else {
    snprintf(msg, sizeof(msg),
      "⚙️ MODE: %s → %s\n🔁 Relays: (no change)",
      oldAuto ? "AUTO" : "MANUAL",
      newAuto ? "AUTO" : "MANUAL"
    );
  }

  Telegram_Enqueue(msg);
}

static bool firstRun=true;
static bool lastAuto=false;
static bool lastR[4]={0,0,0,0};

// ===== Alert state & per-type timers =====
static bool warnTemp = false;
static bool warnSoil = false;
static bool warnLux  = false;
static bool warnPH   = false;
static bool warnRain    = false;

static uint32_t lastTempMs = 0;
static uint32_t lastSoilMs = 0;
static uint32_t lastLuxMs  = 0;
static uint32_t lastPHMs   = 0;
static uint32_t lastRainMs = 0;

static const uint32_t REPEAT_MS = 300000; // 5 phút

static inline void sendLine(const char* fmt, ...){
  char m[240]; va_list ap; va_start(ap,fmt); vsnprintf(m,sizeof(m),fmt,ap); va_end(ap);
  if (Telegram_Enqueue(m)) vTaskDelay(pdMS_TO_TICKS(60));
}

void TaskAlerts(void *pv){
  (void)pv;
  vTaskDelay(pdMS_TO_TICKS(5000)); // cho hệ thống ổn định

  for(;;){
    // ===== Mirror MODE/RELAY =====
    if (firstRun){
      lastAuto=g_autoMode; lastR[0]=r1; lastR[1]=r2; lastR[2]=r3; lastR[3]=r4; firstRun=false;
    } else {
      bool nowAuto=g_autoMode; bool nowR[4]={r1,r2,r3,r4};
      if(nowAuto!=lastAuto){
        vTaskDelay(pdMS_TO_TICKS(300)); // cho auto settle
        nowR[0]=r1; nowR[1]=r2; nowR[2]=r3; nowR[3]=r4;
        sendModeAndRelayDeltaOnce(lastAuto,nowAuto,lastR,nowR);
        lastAuto=nowAuto; for(int i=0;i<4;i++) lastR[i]=nowR[i];
        vTaskDelay(pdMS_TO_TICKS(60));
      } else {
        bool changed = (nowR[0]!=lastR[0])||(nowR[1]!=lastR[1])||(nowR[2]!=lastR[2])||(nowR[3]!=lastR[3]);
        if(changed){
          char stamp[32]; fmtStamp(stamp,sizeof(stamp));
          char line[220]; line[0]='\0';
          for(int i=0;i<4;i++) if(nowR[i]!=lastR[i]){ char seg[42];
            snprintf(seg,sizeof(seg),"%s %s→%s  ",relayName(i+1),lastR[i]?"ON":"OFF",nowR[i]?"ON":"OFF");
            strncat(line,seg,sizeof(line)-strlen(line)-1);
          }
          char msg[320]; snprintf(msg,sizeof(msg),"🔧 Relay change @ %s\n%s",stamp,line);
          Telegram_Enqueue(msg);
          for(int i=0;i<4;i++) lastR[i]=nowR[i];
          vTaskDelay(pdMS_TO_TICKS(60));
        }
      }
    }

    char stamp[32]; fmtStamp(stamp,sizeof(stamp));
    uint32_t now = millis();

    // --- Temperature ---
    if (g_tempC >= Temp_High){
      if (!warnTemp){
        sendLine("\n[ALERT]%s Temp HIGH: %d°C (>= %d)",
          g_autoMode?"[AUTO]":"[MANUAL]", g_tempC, Temp_High);
        warnTemp=true; lastTempMs=now;
      } else if (now - lastTempMs >= REPEAT_MS){
        sendLine("\n[ALERT]%s Temp still HIGH: %d°C (>= %d)",
          g_autoMode?"[AUTO]":"[MANUAL]", g_tempC, Temp_High);
        lastTempMs=now;
      }
    } else if (warnTemp && g_tempC <= Temp_High - 2){
      sendLine("[OK]%s Temp back to normal: %d°C",
        g_autoMode?"[AUTO]":"[MANUAL]", g_tempC);
      warnTemp=false; lastTempMs=0;
    }

    // --- Soil moisture ---
    if (g_moist <= Soil_Low){
      if (!warnSoil){
        sendLine("[ALERT]%s Soil moisture LOW: %d%% (<= %d)",
          g_autoMode?"[AUTO]":"[MANUAL]", g_moist, Soil_Low);
        warnSoil=true; lastSoilMs=now;
      } else if (now - lastSoilMs >= REPEAT_MS){
        sendLine("[ALERT]%s Soil moisture still LOW: %d%% (<= %d)",
          g_autoMode?"[AUTO]":"[MANUAL]", g_moist, Soil_Low);
        lastSoilMs=now;
      }
    } else if (warnSoil && g_moist >= Soil_Low + 5){
      sendLine("[OK]%s Soil restored: %d%%",
        g_autoMode?"[AUTO]":"[MANUAL]", g_moist);
      warnSoil=false; lastSoilMs=0;
    }

    // --- Lux (báo thiếu sáng: g_lux < Lux_Low) ---
    if (g_lux < Lux_Low){
      if (!warnLux){
        sendLine("[ALERT]%s Lux LOW: %d lx (< %d)",
          g_autoMode?"[AUTO]":"[MANUAL]", g_lux, Lux_Low);
        warnLux=true; lastLuxMs=now;
      } else if (now - lastLuxMs >= REPEAT_MS){
        sendLine("[ALERT]%s Lux still LOW: %d lx (< %d)",
          g_autoMode?"[AUTO]":"[MANUAL]", g_lux, Lux_Low);
        lastLuxMs=now;
      }
    } else if (warnLux && g_lux >= Lux_Low + 200){
      sendLine("[OK]%s Lux sufficient: %d lx",
        g_autoMode?"[AUTO]":"[MANUAL]", g_lux);
      warnLux=false; lastLuxMs=0;
    }

    // --- pH ---
    if (g_ph < PH_MIN || g_ph > PH_MAX){
      if (!warnPH){
        sendLine("[ALERT]%s pH OUT of range: %.1f (valid %.1f–%.1f)",
          g_autoMode?"[AUTO]":"[MANUAL]", g_ph, PH_MIN, PH_MAX);
        warnPH = true; lastPHMs = now;
      } else if (now - lastPHMs >= REPEAT_MS){
        sendLine("[ALERT]%s pH still OUT of range: %.1f (valid %.1f–%.1f)",
          g_autoMode?"[AUTO]":"[MANUAL]", g_ph, PH_MIN, PH_MAX);
        lastPHMs = now;
      }
    } else if (warnPH && g_ph >= PH_MIN && g_ph <= PH_MAX){
      sendLine("[OK]%s pH back to normal: %.1f",
        g_autoMode?"[AUTO]":"[MANUAL]", g_ph);
      warnPH = false; lastPHMs = 0;
    }

    // --- Rain (mưa) ---
    if (g_rain) {
      if (!warnRain) {
        sendLine("[ALERT]%s RAIN DETECTED (rain=1)",
          g_autoMode ? "[AUTO]" : "[MANUAL]");
        warnRain = true;
        lastRainMs = now;
      } else if (now - lastRainMs >= REPEAT_MS) {
        sendLine("[ALERT]%s RAIN still ACTIVE (rain=1)",
          g_autoMode ? "[AUTO]" : "[MANUAL]");
        lastRainMs = now;
      }
    } else if (warnRain && !g_rain) {
      sendLine("[OK]%s Rain stopped (rain=0)",
        g_autoMode ? "[AUTO]" : "[MANUAL]");
      warnRain = false;
      lastRainMs = 0;
    }

    // --- SUN (NẮNG GẮT) ---
    if (g_lux >= NANG_GAT_LUX) {
      if (!warnSun) {
        sendLine("[ALERT]%s SUN STRONG: %d lux (>= %d)",
          g_autoMode ? "[AUTO]" : "[MANUAL]",
          g_lux, NANG_GAT_LUX);
        warnSun = true;
        lastSunMs = now;
      }
      else if (now - lastSunMs >= REPEAT_MS) {
        sendLine("[ALERT]%s SUN still STRONG: %d lux",
          g_autoMode ? "[AUTO]" : "[MANUAL]",
          g_lux);
        lastSunMs = now;
      }
    }
    else if (warnSun && g_lux < NANG_GAT_LUX - 10000) { // hysteresis chống rung LUX = 10000
      sendLine("[OK]%s Sun back to normal: %d lux",
        g_autoMode ? "[AUTO]" : "[MANUAL]",
        g_lux);
      warnSun = false;
      lastSunMs = 0;
    }

    // --- Buzzer: Beep 1 lần mỗi 1000ms khi có cảnh báo ---
    static uint32_t lastBeep = 0;
    bool anyAlert = warnTemp || warnSoil || warnLux || warnPH;

    if (g_buzzerEnabled && anyAlert) {
      uint32_t now = millis();

      // Mỗi 1000ms kêu 1 phát
      if (now - lastBeep >= 1000) {
        lastBeep = now;

        // Bật buzzer rất ngắn (50ms)
        digitalWrite(BUZZER_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(50));
        digitalWrite(BUZZER_PIN, LOW);
      }
    } else {
      digitalWrite(BUZZER_PIN, LOW); // đảm bảo tắt buzzer khi hết alert
    }

    vTaskDelay(pdMS_TO_TICKS(300));
  }
}
