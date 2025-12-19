// =============================================
// RTC.cpp — WiFi primary, DS3231 backup (DROP-IN)
// =============================================
#include "RTC.h"
#include <WiFi.h>
#include <Wire.h>
#include "time.h"

// ===== Internal config =====
#ifndef RTC_SDA_PIN
#define RTC_SDA_PIN 21
#endif
#ifndef RTC_SCL_PIN
#define RTC_SCL_PIN 22
#endif

// Accept only valid epoch (>= 2021-01-01) to avoid 01/01/2000
static const time_t MIN_VALID_EPOCH = 1609459200;

// Write DS3231 every 15 minutes when WiFi time is authoritative
static const uint32_t ADJUST_BACK_MS = 15UL * 60UL * 1000UL;

bool RTC_Present() {
  Wire.beginTransmission(0x68);  // DS3231 I2C address
  return (Wire.endTransmission() == 0);
}

void RTC_Init() {
  rtc.begin(); // không fail nếu không cắm DS3231
}

// ---- NTP sync -> set system time -> update DS3231 if present ----
bool RTC_SyncNTP(uint32_t timeout_ms) {
  // Multiple servers for robustness
  extern const long  gmtOffset_sec;
  extern const int   daylightOffset_sec;
  configTime(gmtOffset_sec, daylightOffset_sec,
             "time.google.com", "pool.ntp.org", "time.cloudflare.com");

  struct tm ti;
  uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    time_t now = time(nullptr);
    if (now >= MIN_VALID_EPOCH && getLocalTime(&ti, 300)) {
      if (RTC_Present()) {
        // Ghi ngay DS3231 theo GIỜ ĐỊA PHƯƠNG (đã áp offset) để offline không lệch múi giờ
        rtc.adjust(DateTime(ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                            ti.tm_hour, ti.tm_min, ti.tm_sec));
      }
      return true;
    }
    delay(200);
  }
  return false;
}

void RTC_BuildStrings(DateTime now, String &dateStr, String &timeStr, String &isoStr) {
  char buf[32];

  // DD/MM/YYYY
  snprintf(buf, sizeof(buf), "%02d/%02d/%04d", now.day(), now.month(), now.year());
  dateStr = String(buf);

  // HH:MM:SS
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  timeStr = String(buf);

  // YYYY-MM-DDTHH:MM:SS (ISO-like)
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());
  isoStr = String(buf);
}

void TaskRTC(void *pvParameters) {
  uint32_t lastAdjust = 0;
  bool adjustedOnce = false;

  for (;;) {
    bool haveWifi = (WiFi.status() == WL_CONNECTED);
    time_t epochNow = time(nullptr);

    // Nếu có Wi-Fi nhưng system time chưa hợp lệ -> thử sync
    if (haveWifi && epochNow < MIN_VALID_EPOCH) {
      RTC_SyncNTP(15000);
      epochNow = time(nullptr);
    }

    extern char g_date[11];
    extern char g_time[9];

    struct tm ti;
    if (getLocalTime(&ti, 10) && epochNow >= MIN_VALID_EPOCH) {
      // Ưu tiên SYSTEM TIME (đã áp timezone) kể cả khi mất Wi-Fi, miễn còn hợp lệ
      snprintf(g_date, sizeof(g_date), "%02d/%02d/%04d", ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
      snprintf(g_time, sizeof(g_time), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);

      // Nếu vừa có Wi-Fi lần đầu (hoặc sau reboot) -> GHI NGAY DS3231 một lần
      if (haveWifi && !adjustedOnce && RTC_Present()) {
        rtc.adjust(DateTime(ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                            ti.tm_hour, ti.tm_min, ti.tm_sec));
        adjustedOnce = true;
        lastAdjust = millis();
      }

      // Nếu Wi-Fi đang có -> ghi ngược định kỳ 15 phút để DS3231 luôn khớp giờ địa phương
      if (haveWifi) {
        uint32_t nowMs = millis();
        if ((nowMs - lastAdjust) >= ADJUST_BACK_MS && RTC_Present()) {
          rtc.adjust(DateTime(ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                              ti.tm_hour, ti.tm_min, ti.tm_sec));
          lastAdjust = nowMs;
        }
      }
    }
    else if (RTC_Present()) {
      // Fallback DS3231 khi system time chưa hợp lệ
      DateTime now = rtc.now();
      snprintf(g_date, sizeof(g_date), "%02u/%02u/%04u", now.day(), now.month(), now.year());
      snprintf(g_time, sizeof(g_time), "%02u:%02u:%02u", now.hour(), now.minute(), now.second());
    }
    else {
      // Không có cả Wi-Fi lẫn DS3231
      strcpy(g_date, "--/--/----");
      strcpy(g_time, "--:--:--");
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
