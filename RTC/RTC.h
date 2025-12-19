// =============================================
// RTC.h — WiFi primary, DS3231 backup (DROP-IN)
// =============================================
#ifndef RTC_H
#define RTC_H

#include <Arduino.h>
#include <RTClib.h>

// Provided by your .ino
extern RTC_DS3231 rtc;
extern char g_time[9];     // "HH:MM:SS"
extern char g_date[11];    // "DD/MM/YYYY"
extern const char* ntpServer;          // (giữ cho tương thích; có thể không dùng)
extern const long  gmtOffset_sec;      // e.g. 7*3600
extern const int   daylightOffset_sec; // usually 0

void RTC_Init();
bool RTC_SyncNTP(uint32_t timeout_ms = 15000);
void TaskRTC(void *pvParameters);
void RTC_BuildStrings(DateTime now, String &dateStr, String &timeStr, String &isoStr);
bool RTC_Present();
#endif
